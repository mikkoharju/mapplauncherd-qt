/***************************************************************************
**
** Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (directui@nokia.com)
**
** This file is part of applauncherd
**
** If you have questions regarding the use of this file, please contact
** Nokia at directui@nokia.com.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include <QQuickView>
#include <QQmlEngine>
#include <QtPlugin>
#include <QPluginLoader>
#include <QLibraryInfo>
#include <QGuiApplication>
#include <QFileInfo>
#include <QDir>

#include <private/qcoreapplication_p.h>
#include <sys/types.h>
#include <unistd.h>

#include "mdeclarativecache.h"
#include "mdeclarativecache_p.h"

MDeclarativeCachePrivate * const MDeclarativeCache::d_ptr = new MDeclarativeCachePrivate;
const int MDeclarativeCachePrivate::ARGV_LIMIT = 32;

MDeclarativeCachePrivate::MDeclarativeCachePrivate() :
    qApplicationInstance(0),
    qQuickViewInstance(0),
    qQmlEngine(0),
    initialArgc(ARGV_LIMIT),
    initialArgv(new char* [initialArgc]),
    appDirPath(QString()),
    appFilePath(QString()),
    cachePopulated(false),
    testabilityInterface(0)
{
}

MDeclarativeCachePrivate::~MDeclarativeCachePrivate()
{
    delete qQuickViewInstance;
    delete[] initialArgv;
}

void MDeclarativeCachePrivate::populate()
{
    // Record the fact that the cache has been populated
    cachePopulated = true;
    
    static const char *const emptyString = "";
    static const QString appNameFormat = "mdeclarativecache_pre_initialized_qapplication-%1";
    static QByteArray appName;

    // Append pid to appName to make it unique. This is required because the
    // libminputcontext.so instantiates MComponentData, which in turn registers
    // a dbus service with the application's name.
    appName = appNameFormat.arg(getpid()).toLatin1();

    // We support at most ARGV_LIMIT arguments in QCoreApplication. These will be set when real
    // arguments are known (in MDeclarativeCachePrivate::qApplication). 
    initialArgv[0] = const_cast<char *>(appName.constData());
    for (int i = 1; i < initialArgc; i++) {
        initialArgv[i] = const_cast<char *>(emptyString);
    }

    if (qApplicationInstance == 0) {
        qApplicationInstance = new QGuiApplication(initialArgc, initialArgv);
    }

    if (qQmlEngine)
        qQuickViewInstance = new QQuickView(qQmlEngine, 0);
    else
        qQuickViewInstance = new QQuickView;
}

QGuiApplication *MDeclarativeCachePrivate::qApplication(int &argc, char **argv)
{
    if (qApplicationInstance == 0) 
    {
        qApplicationInstance = new QGuiApplication(argc, argv);
    } 
    else 
    {
        QCoreApplicationPrivate *qap = static_cast<QCoreApplicationPrivate*>(QObjectPrivate::get(qApp));
        if (argc > ARGV_LIMIT) 
        {
            qWarning("MDeclarativeCache: QCoreApplication::arguments() will not contain all arguments.");
        }
        
        // Copy arguments to QCoreApplication 
        for (int i = 0; i < qMin(argc, ARGV_LIMIT); i++) 
        {
            qap->argv[i] = argv[i];
        }
        
        // This changes argc in QCoreApplication
        initialArgc = qMin(argc, ARGV_LIMIT);

        // XXX: Call QCoreApplication::applicationFilePath() once. Internally in QCoreApp::appFilePath()
        // there is a 'static const char *procname' which we need to have cached to 'our' argv[0];
        QCoreApplication::applicationFilePath();

        // Now override appFilePath and appDirPath. These are cached individually, so we
        // need to reset both.
        QFileInfo appFileInfo(argv[0]);
        QCoreApplicationPrivate::setApplicationFilePath(appFileInfo.canonicalFilePath());
        qap->cachedApplicationDirPath = appFileInfo.canonicalPath();

        // Set object name to the name of the binary.
        qApp->setObjectName(appFileInfo.fileName());

        bool loadTestabilityArg = false;
        const char* testabilityArg = "-testability";
        for (int i = 0; i < argc; i++) 
        {
            if (strcmp(argv[i], testabilityArg) == 0)
            {
                loadTestabilityArg = true;
                break;
            }
        }
        
        bool loadTestabilityEnv = !qgetenv("QT_LOAD_TESTABILITY").isNull();
        if (loadTestabilityEnv || loadTestabilityArg)
            testabilityInit();

        if (cachePopulated)
        {
            // In Qt 4.7, QCoreApplication::applicationDirPath() and
            // QCoreApplication::applicationFilePath() look up the paths in /proc,
            // which does not work when the booster is used. As a workaround, we
            // use argv[0] to provide the correct values in the cache class.
            appFilePath = QString(argv[0]);
            appDirPath = QString(argv[0]);
            appDirPath.chop(appDirPath.size() - appDirPath.lastIndexOf("/"));
        }
    }

#ifdef HAVE_PATH_REINIT
    // Set the magic attribute so that paths are reinitialized
    qApplicationInstance->setAttribute(Qt::AA_LinuxReinitPathsFromArgv0, true);
#endif

    return qApplicationInstance;
}

void MDeclarativeCachePrivate::testabilityInit()
{
    // Activate testability plugin if exists
    QString testabilityPluginPostfix = ".so";
    QString testabilityPlugin = "testability/libtestability";

    testabilityPlugin = QLibraryInfo::location(QLibraryInfo::PluginsPath) + QDir::separator() + testabilityPlugin + testabilityPluginPostfix;
    QPluginLoader loader(testabilityPlugin.toLatin1().data());

    QObject *plugin = loader.instance();
    
    if (plugin) 
    {
        testabilityInterface = qobject_cast<TestabilityInterface *>(plugin);

        if (testabilityInterface) 
        {
            testabilityInterface->Initialize();
        }
    }
}


QQuickView *MDeclarativeCachePrivate::qQuickView()
{
    QQuickView *returnValue = 0;
    if (qQuickViewInstance != 0) {
        returnValue = qQuickViewInstance;
        qQuickViewInstance = 0;
    } else if (qQmlEngine)
            returnValue = new QQuickView(qQmlEngine, 0);
        else
            returnValue = new QQuickView;

    // Give ownership of the engine to the created quickview
    if (qQmlEngine)
        qQmlEngine->setParent(returnValue);

    return returnValue;
}

QString MDeclarativeCachePrivate::applicationDirPath()
{
    if (cachePopulated) {
        // In the booster case use the workaround
        return appDirPath;
    } else {
        return QCoreApplication::applicationDirPath();
    }
}

QString MDeclarativeCachePrivate::applicationFilePath()
{
    if (cachePopulated) {
        // In the booster case use the workaround
        return appFilePath;
    } else {
        return QCoreApplication::applicationFilePath();
    }
}

QQuickView *MDeclarativeCache::populate()
{
    d_ptr->populate();
    return d_ptr->qQuickViewInstance;
}

QGuiApplication *MDeclarativeCache::qApplication(int &argc, char **argv)
{    
    return d_ptr->qApplication(argc, argv);
}

QQuickView *MDeclarativeCache::qQuickView()
{
    return d_ptr->qQuickView();
}

QString MDeclarativeCache::applicationDirPath()
{
    return d_ptr->applicationDirPath();
}

QString MDeclarativeCache::applicationFilePath()
{
    return d_ptr->applicationFilePath();
}

void MDeclarativeCache::setQQmlEngine(QQmlEngine *engine)
{
    d_ptr->qQmlEngine = engine;
}

