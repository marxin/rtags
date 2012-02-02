#include "RBuild.h"
#include <RTags.h>
#include <QCoreApplication>
#include <QtAlgorithms>
#include <sstream>
#include <clang-c/Index.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include "RBuild_p.h"
#include <FileDB.h>
#include <memory>

QHash<Path, unsigned> *filesByName = 0;

using namespace RTags;

static QElapsedTimer timer;

class CompileRunnable : public QRunnable
{
public:
    CompileRunnable(RBuild *rb, const GccArguments &a)
        : rbuild(rb), args(a)
    {
        setAutoDelete(true);
    }

    virtual void run()
    {
        const qint64 before = timer.elapsed();
        rbuild->compile(args);
        const qint64 elapsed = timer.elapsed();
        fprintf(stderr, "parsed %s, (%lld ms)\n", args.input().constData(), elapsed - before);
    }
private:
    RBuild *rbuild;
    const GccArguments args;
};

static inline bool removeGch(QByteArray &pch)
{
    const int idx = pch.indexOf(".gch");
    if (idx != -1) {
        pch.remove(idx, pch.size() - idx);
        return true;
    }
    return false;
}

static inline bool isSourceDirty(const Source* source, QSet<Path>* dirty)
{
    const Path &input = source->args.input();
    if (source->fromUnsavedFile) {
        dirty->insert(input);
        return true;
    }

    if (dirty->contains(input))
        return true;
    bool dirtySource = (input.lastModified() != source->lastModified);
    for (QHash<Path, quint64>::const_iterator it = source->dependencies.constBegin();
         it != source->dependencies.constEnd(); ++it) {
        if (dirty && dirty->contains(it.key())) {
            dirtySource = true;
        } else if (it.key().lastModified() != it.value()) {
            if (dirty)
                dirty->insert(it.key());
            dirtySource = true;
        }
    }
    return dirtySource;
}

RBuild::RBuild(unsigned flags, QObject *parent)
    : QObject(parent), mData(new RBuildPrivate)
{
    mData->flags = flags;
    mData->index = clang_createIndex(1, 0);
    if (const char *env = getenv("RTAGS_THREAD_COUNT")) {
        const int threads = atoi(env);
        if (threads > 0)
            mData->threadPool.setMaxThreadCount(threads);
    }
    mData->systemIncludes = RTags::systemIncludes();
    connect(this, SIGNAL(compileFinished(bool)), this, SLOT(onCompileFinished()));
    timer.start();
}

RBuild::~RBuild()
{
    clang_disposeIndex(mData->index);
    delete mData;
}

void RBuild::setDBPath(const Path &path)
{
    mData->dbPath = path;
}

bool RBuild::buildDB(const QList<Path> &makefiles,
                     const QList<Path> &sources,
                     const Path &sourceDir)
{
    mData->sourceDir = sourceDir.isEmpty() ? Path(".") : sourceDir;
    mData->sourceDir.resolve();
    if (!mData->sourceDir.isDir()) {
        fprintf(stderr, "%s is not a directory\n", sourceDir.constData());
        return false;
    }
    if (!mData->sourceDir.endsWith('/'))
        mData->sourceDir.append('/');

    bool ok = false;
    foreach(const Path &makefile, makefiles) {
        if (makefile.isFile()) {
            MakefileParser *parser = new MakefileParser(mData->flags & Verbose, this);
            connect(parser, SIGNAL(fileReady(GccArguments)),
                    this, SLOT(processFile(GccArguments)));
            connect(parser, SIGNAL(done()), this, SLOT(onMakefileDone()));
            mData->makefileParsers.insert(parser);
            parser->run(makefile);
            ok = true;
        } else {
            qWarning("%s is not a Makefile", makefile.constData());
            return false;
        }
    }
    foreach(const Path &source, sources) {
        if (!source.isFile()) {
            qWarning("%s is not a file", source.constData());
            return false;
        }

        const GccArguments::Language lang = GccArguments::guessLanguage(source);
        char buf[1024];
        const int written = snprintf(buf, 1024, "gcc %s%s %s",
                                     lang != GccArguments::LangUndefined ? "-x" : "",
                                     (lang != GccArguments::LangUndefined
                                      ? GccArguments::languageString(lang)
                                      : ""),
                                     source.constData());
        Q_ASSERT(written < 1024);

        GccArguments args;
        if (!args.parse(QByteArray(buf, written))) {
            qWarning("Couldn't parse %s", buf);
            return false;
        } else {
            processFile(args);
            ok = true;
        }
    }

    if (!ok) {
        qWarning("Nothing to do");
        return false;
    }
    QEventLoop loop;
    connect(this, SIGNAL(finishedCompiling()), &loop, SLOT(quit()));
    loop.exec();
    save();
    return true;
}

bool RBuild::updateDB(const QHash<Path, QByteArray> &unsavedFiles)
{
    if (!openDB(Update))
        return false;
#ifdef QT_DEBUG
    const int beforeCount = mData->db->count();
#endif
    QList<Source> sources = mData->db->read<QList<Source> >("sources");
    mData->pch = mData->db->read<QHash<QByteArray, QPair<Path, Path> > >("pch");
    mData->filesByName = mData->db->read<QHash<Path, unsigned> >("filesByName");
    mData->unsavedFiles.resize(unsavedFiles.size());
    mData->unsavedFilesHash = unsavedFiles;

    QSet<Path> dirty;
    int u = 0;
    for (QHash<Path, QByteArray>::const_iterator it = unsavedFiles.begin();
         it != unsavedFiles.end(); ++it) {
        dirty.insert(it.key());
        mData->unsavedFiles[u].Filename = it.key().constData();
        mData->unsavedFiles[u].Contents = it.value().constData();
        mData->unsavedFiles[u].Length = it.value().size();
        ++u;
    }

    QList<Source*> reparse;
    const int sourceCount = sources.size();
    for (int i=0; i<sourceCount; ++i) {
        const Source &source = sources.at(i);
        if (isSourceDirty(&source, &dirty)) {
            switch (source.args.language()) {
            case GccArguments::LangCPlusPlusHeader:
            case GccArguments::LangHeader: {
                QByteArray key = source.args.output();
                if (removeGch(key))
                    mData->pch.remove(key);
                break; }
            default:
                break;
            }
            dirty.insert(source.args.input());
            reparse.append(&sources[i]);
        } else {
            mData->sources.append(source);
        }
    }
    if (reparse.isEmpty()) {
        printf("Nothing has changed (%lld ms)\n", timer.elapsed());
        return true;
    }

    mData->db->invalidateEntries(dirty);
    foreach(const Source *source, reparse) {
        processFile(source->args);
    }
    QEventLoop loop;
    connect(this, SIGNAL(finishedCompiling()), &loop, SLOT(quit()));
    loop.exec();
    writeEntities();
    writePch();
    mData->db->write("sources", mData->sources);

    printf("Updated db %lld ms, %d threads\n",
           timer.elapsed(), mData->threadPool.maxThreadCount());
#ifdef QT_DEBUG
    printf("%d => %d entries\n", beforeCount, closeDB());
#else
    closeDB();
#endif

    return true;
}

void RBuild::save()
{
    const qint64 beforeWriting = timer.elapsed();

    // Q_ASSERT(filename.endsWith(".rtags.db"));
    if (!openDB(Create)) {
        return;
    }
    writeData();
    closeDB();
    const qint64 elapsed = timer.elapsed();
    fprintf(stderr, "All done. (total/saving %lld/%lld ms, %d threads\n",
            elapsed, elapsed - beforeWriting, mData->threadPool.maxThreadCount());
}

void RBuild::processFile(const GccArguments& arguments)
{
    switch (arguments.language()) {
    case GccArguments::LangHeader:
    case GccArguments::LangCPlusPlusHeader:
        pch(arguments);
        return;
    default:
        break;
    }
    const Path input = arguments.input();
    QSet<QByteArray> defines;
    foreach(const GccArguments &a, mData->files) {
        if (a.input() == input && arguments.language() == a.language()) {
            if (defines.isEmpty())
                defines = arguments.arguments("-D").toSet();
            if (a.arguments("-D").toSet() == defines) {
                return; // duplicate
            }
        }
    }
    mData->files.append(arguments);

    ++mData->pendingJobs;
    mData->threadPool.start(new CompileRunnable(this, arguments));
}

static void recurseDir(QSet<Path> *allFiles, Path path, int rootDirLen)
{
#if defined(_DIRENT_HAVE_D_TYPE) || defined(Q_OS_BSD4) || defined(Q_OS_SYMBIAN)
    DIR *d = opendir(path.constData());
    char fileBuffer[PATH_MAX];
    if (d) {
        if (!path.endsWith('/'))
            path.append('/');
        dirent *p;
        while ((p=readdir(d))) {
            switch (p->d_type) {
            case DT_DIR:
                if (p->d_name[0] != '.') {
                    recurseDir(allFiles, path + QByteArray::fromRawData(p->d_name, strlen(p->d_name)), rootDirLen);
                }
                break;
            case DT_REG: {
                const int w = snprintf(fileBuffer, PATH_MAX, "%s%s", path.constData() + rootDirLen, p->d_name);
                if (w >= PATH_MAX) {
                    fprintf(stderr, "Path too long: %d, max is %d\n", w, PATH_MAX);
                } else {
                    allFiles->insert(Path(fileBuffer, w));
                }
                break; }
                // case DT_LNK: not following links
            }

        }
        closedir(d);
    }
#else
#warning "Can't use --source-dir on this platform"
#endif
}

void RBuild::writeData()
{
    mData->db->write("filesByName", mData->filesByName);
    writeEntities();
    mData->db->write("sources", mData->sources);
    writePch();
    mData->db->write("pch", mData->pch);
    QSet<Path> allFiles;

    if (!mData->sourceDir.isEmpty()) {
        Q_ASSERT(mData->sourceDir.endsWith('/'));
        if (mData->sourceDir.isDir()) {
            recurseDir(&allFiles, mData->sourceDir, mData->sourceDir.size());
        } else {
            fprintf(stderr, "%s is not a directory\n", mData->sourceDir.constData());
        }
        mData->db->write("sourceDir", mData->sourceDir);
    }
    mData->db->write("files", allFiles);
}

static inline void debugCursor(FILE* out, const CXCursor& cursor)
{
    CXFile file;
    unsigned int line, col, off;
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    clang_getInstantiationLocation(loc, &file, &line, &col, &off);
    CXString name = clang_getCursorDisplayName(cursor);
    CXString filename = clang_getFileName(file);
    CXString kind = clang_getCursorKindSpelling(clang_getCursorKind(cursor));
    fprintf(out, "cursor name %s, kind %s, loc %s:%u:%u\n",
            clang_getCString(name), clang_getCString(kind),
            clang_getCString(filename), line, col);
    clang_disposeString(name);
    clang_disposeString(kind);
    clang_disposeString(filename);
}

// #define COLLECTDEBUG

struct InclusionUserData {
    InclusionUserData(RBuildPrivate *priv,
                      const Path &f,
                      QHash<Path, quint64> &deps,
                      bool &u)
        : mData(priv), file(f), dependencies(deps), unsaved(u)
    {}
    RBuildPrivate *mData;
    const Path &file;
    QHash<Path, quint64> &dependencies;
    bool &unsaved;
};

static inline bool isSystemInclude(const Path &header, const QList<QByteArray> &systemIncludes)
{
    foreach(const QByteArray &systemInclude, systemIncludes) {
        if (!strncmp(header.constData(), systemInclude.constData() + 2, systemInclude.size() - 2))
            return true;
    }
    return false;
}

void RBuild::getInclusions(CXFile includedFile,
                           CXSourceLocation* inclusionStack,
                           unsigned inclusionStackLen,
                           CXClientData userData)
{
    InclusionUserData *u = reinterpret_cast<InclusionUserData*>(userData);
    if (u->unsaved)
        return;
    if (u->mData->flags & Verbose) {
        printf("getInclusions %s %d for %s\n",
               eatString(clang_getFileName(includedFile)).constData(),
               inclusionStackLen, u->file.constData());
        for (unsigned i=0; i<inclusionStackLen; ++i) {
            CXFile f;
            clang_getSpellingLocation(inclusionStack[i], &f, 0, 0, 0);
            printf("    %d/%d %s\n", i + 1, inclusionStackLen,
                   eatString(clang_getFileName(f)).constData());
        }
    }

    if (inclusionStackLen) {
        CXString str = clang_getFileName(includedFile);
        const char *cstr = clang_getCString(str);
        if (!(u->mData->flags & EnableSystemHeaderDependencies) || strncmp("/usr/", cstr, 5) != 0) {
            const Path p = Path::resolved(cstr);
            if (u->mData->unsavedFilesHash.contains(p)) {
                u->unsaved = true;
            } else {
                bool add = true;
                if (!(u->mData->flags & EnableSystemHeaderDependencies)) {
                    foreach(const QByteArray &systemInclude, u->mData->systemIncludes) {
                        if (!strncmp(p.constData(), systemInclude.constData() + 2, systemInclude.size() - 2)) {
                            add = false;
                            break;
                        }
                    }
                }
                if (add) {
                    u->dependencies[p] = p.lastModified();
                }
            }
        }
        clang_disposeString(str);
    }
}

static inline Location createLocation(const CXIdxLoc &l, QHash<Path, unsigned> &files)
{
    Location loc;
    CXFile f;
    clang_indexLoc_getFileLocation(l, 0, &f, &loc.line, &loc.column, 0);
    CXString str = clang_getFileName(f);
    const Path fileName = Path::resolved(clang_getCString(str));
    unsigned &file = files[fileName];
    if (!file)
        file = files.size();
    loc.file = file;
    clang_disposeString(str);
    return loc;
}

static inline Location createLocation(const CXCursor &c, QHash<Path, unsigned> &files)
{
    Location loc;
    CXFile f;
    CXSourceLocation sl = clang_getCursorLocation(c);
    clang_getInstantiationLocation(sl, &f, &loc.line, &loc.column, 0);
    CXString str = clang_getFileName(f);
    const Path fileName = Path::resolved(clang_getCString(str));
    unsigned &file = files[fileName];
    if (!file)
        file = files.size();
    loc.file = file;
    clang_disposeString(str);
    return loc;
}

struct UserData {
    RBuildPrivate *p;
    Path file;
};

// This function is always called with the lock held
CXChildVisitResult RBuild::visitor(CXCursor cursor, CXCursor, CXClientData userData)
{
    switch (clang_getCursorKind(cursor)) {
    case CXCursor_ParmDecl:
        return CXChildVisit_Recurse;
    case CXCursor_TemplateRef:
    case CXCursor_TypeRef: {
        RBuildPrivate *p = reinterpret_cast<UserData*>(userData)->p;
        if (p->flags & RBuild::DebugAllSymbols) {
            CXCursor r = clang_getCursorReferenced(cursor);
            CXFile f, f2;
            unsigned l, c, l2, c2;
            clang_getInstantiationLocation(clang_getCursorLocation(cursor), &f, &l, &c, 0);
            clang_getInstantiationLocation(clang_getCursorLocation(r), &f2, &l2, &c2, 0);
            const Path p = Path::resolved(eatString(clang_getFileName(f)));
            const Path p2 = Path::resolved(eatString(clang_getFileName(f2)));
            printf("%s:%d:%d: vref of %s %s %s:%d:%d\n",
                   p.constData(), l, c,
                   kindToString(clang_getCursorKind(r)).constData(),
                   eatString(clang_getCursorSpelling(r)).constData(),
                   p2.constData(), l2, c2);
        }
        const Location loc = createLocation(cursor, p->filesByName);
        PendingReference &r = p->pendingReferences[loc];
        if (r.usr.isEmpty()) {
            CXCursor ref = clang_getCursorReferenced(cursor);
            r.usr = eatString(clang_getCursorUSR(ref));
            CXCursor spec = clang_getSpecializedCursorTemplate(ref);
            if (!clang_isInvalid(clang_getCursorKind(spec)))
                r.specialized = eatString(clang_getCursorUSR(spec));
            if (clang_getCursorKind(ref) == CXCursor_TemplateTypeParameter) {
                Entity e;
                e.symbolName = eatString(clang_getCursorDisplayName(ref));
                e.definition = createLocation(ref, p->filesByName);
                e.cursorScope = cursorScope(ref);
                p->entities[r.usr] = e;
            }
        }

        break; }
    default:
        break;
    }
    return CXChildVisit_Continue;
}

void RBuild::indexDeclaration(CXClientData userData, const CXIdxDeclInfo *decl)
{
    RBuildPrivate *p = reinterpret_cast<UserData*>(userData)->p;
    if (p->flags & RBuild::DebugAllSymbols) {
        CXFile f;
        unsigned l, c;
        clang_indexLoc_getFileLocation(decl->loc, 0, &f, &l, &c, 0);
        Path p = Path::resolved(eatString(clang_getFileName(f)));
        QByteArray extra;
        if (decl->isDefinition)
            extra += "definition ";
        if (clang_isVirtualBase(decl->cursor))
            extra += "isVirtualBase ";
        CXCursor *overridden = 0;
        unsigned count = 0;
        clang_getOverriddenCursors(decl->cursor, &overridden, &count);
        for (unsigned i=0; i<count; ++i) {
            extra += cursorToString(overridden[i]) + ' ';
        }
        clang_disposeOverriddenCursors(overridden);
        printf("%s:%d:%d: %s %s %s\n",
               p.constData(), l, c, kindToString(decl->entityInfo->kind), decl->entityInfo->name,
               extra.constData());
    }

    QMutexLocker lock(&p->mutex);

    Entity &e = p->entities[decl->entityInfo->USR];
    Location loc = createLocation(decl->loc, p->filesByName);
    if (decl->isDefinition) {
        e.definition = loc;
    } else {
        e.declarations.insert(loc);
    }
    if (e.symbolName.isEmpty()) {
        CXString nm = clang_getCursorDisplayName(decl->cursor); // this one gives us args
        e.symbolName = clang_getCString(nm);
        clang_disposeString(nm);
        if (decl->entityInfo->kind == CXIdxEntity_CXXInstanceMethod || CXIdxEntity_CXXDestructor) {
            CXCursor *overridden = 0;
            unsigned count = 0;
            clang_getOverriddenCursors(decl->cursor, &overridden, &count);
            if (count == 1) { // we don't care about multiple inheritance, we could just take first I guess?
                const Location l = createLocation(overridden[0], p->filesByName);
                e.super = l;
                // qDebug() << "setting super for " << loc << "to" << l;
                const QByteArray usr = eatString(clang_getCursorUSR(overridden[0]));
                QHash<QByteArray, Entity>::iterator it = p->entities.find(usr);
                if (it != p->entities.end()) {
                    // qDebug() << "adding " << loc << "as sub for" << l;
                    it.value().subs.insert(loc);
                }
            }
            clang_disposeOverriddenCursors(overridden);
        }
        e.cursorScope = cursorScope(decl->cursor);
    }
    // qDebug() << loc << name << kindToString(kind) << decl->entityInfo->templateKind
    //          << decl->entityInfo->USR;
    bool visit = false;
    switch (decl->entityInfo->kind) {
    case CXIdxEntity_CXXConstructor:
        visit = true;
    case CXIdxEntity_CXXDestructor: {
        if (decl->entityInfo->kind == CXIdxEntity_CXXDestructor) {
            Q_ASSERT(e.symbolName.startsWith('~'));
            ++loc.column; // this is just for renameSymbol purposes
        }

        Q_ASSERT(decl->semanticContainer);
        CXString usr = clang_getCursorUSR(decl->semanticContainer->cursor);
        p->entities[clang_getCString(usr)].extraDeclarations.insert(loc);
        break; }
    case CXIdxEntity_Function:
    case CXIdxEntity_CXXInstanceMethod:
    case CXIdxEntity_Variable:
    case CXIdxEntity_Field:
        visit = true;
        break;
    default:
        break;
    }
    if (visit) {
        clang_visitChildren(decl->cursor, RBuild::visitor, userData);
    }
}

void RBuild::indexReference(CXClientData userData, const CXIdxEntityRefInfo *ref)
{
    RBuildPrivate *p = reinterpret_cast<UserData*>(userData)->p;
    if (p->flags & RBuild::DebugAllSymbols) {
        CXFile f, f2;
        unsigned l, c, l2, c2;
        clang_indexLoc_getFileLocation(ref->loc, 0, &f, &l, &c, 0);
        CXSourceLocation loc = clang_getCursorLocation(ref->referencedEntity->cursor);
        clang_getInstantiationLocation(loc, &f2, &l2, &c2, 0);
        const Path p = Path::resolved(eatString(clang_getFileName(f)));
        const Path p2 = Path::resolved(eatString(clang_getFileName(f2)));
        printf("%s:%d:%d: ref of %s %s %s:%d:%d\n",
               p.constData(), l, c,
               kindToString(ref->referencedEntity->kind),
               ref->referencedEntity->name,
               p2.constData(), l2, c2);
    }

    QMutexLocker lock(&p->mutex);
    const Location loc = createLocation(ref->loc, p->filesByName);
    PendingReference &r = p->pendingReferences[loc];
    if (r.usr.isEmpty()) {
        r.usr = ref->referencedEntity->USR;
        CXCursor spec = clang_getSpecializedCursorTemplate(ref->referencedEntity->cursor);
        if (!clang_isInvalid(clang_getCursorKind(spec)))
            r.specialized = eatString(clang_getCursorUSR(spec));
        if (clang_getCursorKind(ref->referencedEntity->cursor) == CXCursor_TemplateTypeParameter) {
            Entity e;
            e.symbolName = eatString(clang_getCursorDisplayName(ref->referencedEntity->cursor));
            e.definition = createLocation(ref->referencedEntity->cursor, p->filesByName);
            e.cursorScope = cursorScope(ref->referencedEntity->cursor);
            p->entities[r.usr] = e;
        }
    }
}

void RBuild::diagnostic(CXClientData userdata, CXDiagnosticSet set, void *)
{
    const int count = clang_getNumDiagnosticsInSet(set);
    RBuildPrivate *p = reinterpret_cast<UserData*>(userdata)->p;
    const bool verbose = p->flags & Verbose;
    for (int i=0; i<count; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnosticInSet(set, i);
        const bool error = clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error;
        if (verbose || error) {
            const QByteArray diag = eatString(clang_formatDiagnostic(diagnostic, 0xff));
            if (error) {
                QMutexLocker lock(&p->mutex);
                if (!p->errorFd) {
                    char tmp[256];
                    strncpy(tmp, "/tmp/rtagspch.error.XXXXXX", 255);
                    p->errorFd = mkstemp(tmp);
                    p->errorFn = tmp;
                }
                if (p->errorFd > 0) {
                    char buf[1024];
                    const int w = snprintf(buf, 1024, "%s: %s\n",
                                           reinterpret_cast<UserData*>(userdata)->file.constData(),
                                           diag.constData());
                    int written = 0;
                    while (written < w) {
                        const int wrote = write(p->errorFd, buf + written, w - written);
                        if (wrote <= 0)
                            break;
                        written += wrote;
                    }
                }
            }
            fprintf(stderr, "%s: %s\n",
                    reinterpret_cast<UserData*>(userdata)->file.constData(),
                    diag.constData());
        }
        clang_disposeDiagnostic(diagnostic);
    }
}

bool RBuild::compile(const GccArguments &gccArgs, const Path &output, Source **source)
{
    if (source)
        *source = 0;
    bool ret = true;
    if ((mData->flags & DontClang) != DontClang) {
        const Path file = gccArgs.input();
        QList<QByteArray> args = gccArgs.clangArgs();
        QHash<Path, quint64> pchDependencies;
        bool unsaved = mData->unsavedFilesHash.contains(file);
        switch (gccArgs.language()) {
        case GccArguments::LangCPlusPlusHeader:
        case GccArguments::LangHeader:
            args << "-emit-pch";
            break;
        case GccArguments::LangCPlusPlus: {
            QMutexLocker lock(&mData->mutex);
            foreach(const QByteArray &inc, gccArgs.arguments("-include")) {
                const QPair<Path, Path> p = mData->pch.value(inc);
                if (!p.first.isFile()) {
                    mData->pch.remove(inc);
                } else {
                    args << "-include-pch" << p.first;
                    if (mData->pchFromUnsaved.contains(inc))
                        unsaved = true;
                    if (!unsaved)
                        pchDependencies[p.second] = p.second.lastModified();
                }
            }
            break; }
        default:
            break;
        }
        QVarLengthArray<const char *, 64> clangArgs(args.size()
                                                    + mData->extraArgs.size()
                                                    + mData->systemIncludes.size());
        int argCount = 0;
        foreach(const QByteArray& arg, args) {
            clangArgs[argCount++] = arg.constData();
        }
        foreach(const QByteArray &systemInclude, mData->systemIncludes) {
            clangArgs[argCount++] = systemInclude.constData();
        }
        foreach(const QByteArray &extraArg, mData->extraArgs) {
            clangArgs[argCount++] = extraArg.constData();
        }

        IndexerCallbacks cb;
        memset(&cb, 0, sizeof(IndexerCallbacks));
        cb.diagnostic = diagnostic;
        if ((mData->flags & DontIndex) != DontIndex) {
            cb.indexDeclaration = indexDeclaration;
            cb.indexEntityReference = indexReference;
        }

        CXIndexAction action = clang_IndexAction_create(mData->index);
        CXTranslationUnit unit = 0;
        const bool verbose = (mData->flags & Verbose);

        if (verbose) {
            fprintf(stderr, "clang ");
            for (int i=0; i<argCount; ++i) {
                fprintf(stderr, "%s ", clangArgs[i]);
            }
            fprintf(stderr, "%s\n", file.constData());
        }

        UserData userData = { mData, file };
        clang_indexSourceFile(action, &userData, &cb, sizeof(IndexerCallbacks),
                              CXIndexOpt_IndexFunctionLocalSymbols, file.constData(),
                              clangArgs.constData(), argCount,
                              mData->unsavedFiles.data(), mData->unsavedFiles.size(),
                              &unit, clang_defaultEditingTranslationUnitOptions());
        // ### do we need incomplete for pch?

        if (!unit) {
            qWarning() << "Unable to parse unit for" << file;
            fprintf(stderr, "clang ");
            for (int i=0; i<argCount; ++i) {
                fprintf(stderr, "%s", clangArgs[i]);
                fprintf(stderr, "%c", i + 1 < argCount ? ' ' : '\n');
            }

            ret = false;
        } else {
            Source src = { gccArgs, file.lastModified(), pchDependencies, unsaved };
            if (!unsaved) {
                InclusionUserData u(mData, file, src.dependencies, src.fromUnsavedFile);
                clang_getInclusions(unit, getInclusions, &u);
            }
            if (src.fromUnsavedFile) {
                src.dependencies.clear();
            }
            // qDebug() << file << "depends on" << src.dependencies.keys();
            {
                QMutexLocker lock(&mData->mutex);
                mData->sources.append(src);
                if (source)
                    *source = &mData->sources.last();
            }
            // qDebug() << input << mData->dependencies.last().dependencies.keys();
            if (!output.isEmpty()) {
                const int c = clang_saveTranslationUnit(unit, output.constData(), clang_defaultSaveOptions(unit));
                if (c) {
                    qWarning("Couldn't save translation unit: %d", c);
                    // This will likely upset things
                    ret = false;
                }
            }
            clang_disposeTranslationUnit(unit);
        }
    }
    emit compileFinished(ret);
    return ret;
}

void RBuild::onCompileFinished()
{
    if (!--mData->pendingJobs && mData->makefileParsers.isEmpty())
        emit finishedCompiling();
}

bool RBuild::openDB(Mode mode)
{
    Q_ASSERT(!mData->db);
    mData->db = Database::create(mData->dbPath,
                                 mode == Update ? Database::ReadWrite : Database::WriteOnly);
    return mData->db->isOpened();
}

int RBuild::closeDB()
{
    int ret = -1;
    if (mData->db) {
        ret = mData->db->close();
        if (mData->errorFd) {
            close(mData->errorFd);
            mData->errorFd = 0;
            Path p = mData->db->path();
            time_t t;
            time(&t);
            struct tm *tm;
            tm = localtime(&t);
            char buf[128];
            const int ret = strftime(buf, 80, "/errors_%D_%T.log", tm);
            for (int i=0; i<ret; ++i) {
                if (buf[i] == '/')
                    buf[i] = '_';
            }

            p += QByteArray::fromRawData(buf, ret);
            if (rename(mData->errorFn, p.constData()) == -1) {
                qWarning("Move error %s", strerror(errno));
            }
            qDebug() << "moved" << mData->errorFn << "to" << p;
        }
        delete mData->db;
        mData->db = 0;
    }
    return ret;
}
void RBuild::writeEntities()
{
    for (QHash<Location, PendingReference>::const_iterator it = mData->pendingReferences.begin();
         it != mData->pendingReferences.end(); ++it) {
        const PendingReference &ref = it.value();
        QHash<QByteArray, Entity>::iterator i = mData->entities.find(ref.usr);
        if (i == mData->entities.end())
            i = mData->entities.find(ref.specialized);
        if (i == mData->entities.end()) {
            qDebug() << "Can't find this reference anywhere" << ref.usr << ref.specialized << it.key();
        } else {
            i.value().references.insert(it.key());
        }
    }

    foreach(const Entity &entity, mData->entities) {
        mData->db->writeEntity(entity);
    }
}

// it's only legal to call these before any compilation has started
void RBuild::addDefines(const QList<QByteArray> &defines)
{
    foreach(const QByteArray &define, defines)
        mData->extraArgs += ("-D" + define);
}

void RBuild::addIncludePaths(const QList<Path> &paths)
{
    foreach(const Path &path, paths)
        mData->extraArgs += ("-I" + path);
}
void RBuild::onMakefileDone()
{
    MakefileParser *parser = qobject_cast<MakefileParser*>(sender());
    mData->makefileParsers.remove(parser);
    parser->deleteLater();
    if (mData->makefileParsers.isEmpty() && !mData->pendingJobs)
        emit finishedCompiling();
}

bool RBuild::pch(const GccArguments &pch)
{
    if (mData->flags & DisablePCH)
        return false;
    const qint64 before = timer.elapsed();
    QByteArray output = pch.output();
    if (!removeGch(output))
        return false;
    {
        QMutexLocker lock(&mData->mutex);
        if (mData->pch.contains(output))
            return true;
        mData->pch[output] = QPair<Path, Path>();
    }

    char tmp[128];
    strncpy(tmp, "/tmp/rtagspch.XXXXXX", 127);
    int id = mkstemp(tmp);
    if (id == -1) {
        QMutexLocker lock(&mData->mutex);
        mData->pch.remove(output);
        return false;
    }
    close(id);

    ++mData->pendingJobs;
    Source *src = 0;
    const bool ok = compile(pch, tmp, &src);
    QMutexLocker lock(&mData->mutex);
    if (ok) {
        unsigned &file = mData->filesByName[pch.input()];
        if (!file)
            file = mData->filesByName.size();
        mData->pch[output] = qMakePair(Path(tmp), pch.input());
        if (src->fromUnsavedFile)
            mData->pchFromUnsaved.insert(output);
        const qint64 elapsed = timer.elapsed() - before;
        printf("pch %s %s (%lld ms)\n", pch.input().constData(), output.constData(), elapsed);
    } else {
        mData->pch.remove(output);
    }
    return ok;
}
void RBuild::writePch()
{
    if (!mData->pch.isEmpty())
        ::mkdir((mData->db->path() + "/pch").constData(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

    QHash<QByteArray, QPair<Path, Path> >::iterator it = mData->pch.begin();
    while (it != mData->pch.end()) {
        Path &pch = it.value().first;
        Path p = mData->db->path();
        QByteArray key = it.key();
        key.replace('/', '_'); // hopefully this is always unique ###
        p += "/pch/" + key;
        if (p != pch) {
            // qDebug() << "need to move" << pch << "to" << p;
            unlink(p.constData());
            if (rename(pch.constData(), p.constData()) == -1) {
                it = mData->pch.erase(it);
                qWarning("Move error %s", strerror(errno));
                continue;
            }
            pch = p;
        }
        ++it;
    }
}
QList<QByteArray> RBuild::cursorScope(CXCursor cursor)
{
#warning bug here, all references for scope only gives itself and the scope.prepend thing, not the return statement. This seems to work when its an int though
    QList<QByteArray> scope;
    forever {
        cursor = clang_getCursorSemanticParent(cursor);
        const CXCursorKind k = clang_getCursorKind(cursor);
        if (clang_isInvalid(k))
            break;
        CXString str = clang_getCursorDisplayName(cursor);
        const char *cstr = clang_getCString(str);
        if (!cstr || !strlen(cstr)) {
            clang_disposeString(str);
            break;
        }
        switch (k) {
        case CXCursor_StructDecl:
        case CXCursor_ClassDecl:
        case CXCursor_Namespace:
            scope.prepend(cstr);
            break;
        default:
            break;
        }
        clang_disposeString(str);
    }
    return scope;
}
