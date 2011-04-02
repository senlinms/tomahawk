/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 * 
 *   Copyright 2010-2011, Christian Muehlhaeuser <muesli@tomahawk-player.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "musicscanner.h"

#include "tomahawk/tomahawkapp.h"
#include "sourcelist.h"
#include "database/database.h"
#include "database/databasecommand_dirmtimes.h"
#include "database/databasecommand_addfiles.h"
#include "database/databasecommand_deletefiles.h"

using namespace Tomahawk;


void
DirLister::go()
{
    foreach( QString dir, m_dirs )
        scanDir( QDir( dir, 0 ), 0, ( m_recursive ? DirLister::Recursive : DirLister::NonRecursive ) );
    emit finished( m_newdirmtimes );
}


void
DirLister::scanDir( QDir dir, int depth, DirLister::Mode mode )
{
    qDebug() << "DirLister::scanDir scanning: " << dir.absolutePath();
    QFileInfoList dirs;
    const uint mtime = QFileInfo( dir.absolutePath() ).lastModified().toUTC().toTime_t();
    m_newdirmtimes.insert( dir.absolutePath(), mtime );

    if ( m_dirmtimes.contains( dir.absolutePath() ) && mtime == m_dirmtimes.value( dir.absolutePath() ) )
    {
        // dont scan this dir, unchanged since last time.
    }
    else
    {
        if ( m_dirmtimes.contains( dir.absolutePath() ) )
            Database::instance()->enqueue( QSharedPointer<DatabaseCommand>( new DatabaseCommand_DeleteFiles( dir, SourceList::instance()->getLocal() ) ) );

        dir.setFilter( QDir::Files | QDir::Readable | QDir::NoDotAndDotDot );
        dir.setSorting( QDir::Name );
        dirs = dir.entryInfoList();
        foreach( const QFileInfo& di, dirs )
        {
            emit fileToScan( di );
        }
    }
    dir.setFilter( QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot );
    dirs = dir.entryInfoList();

    foreach( const QFileInfo& di, dirs )
    {
        if( mode == DirLister::Recursive || !m_dirmtimes.contains( di.absolutePath() ) )
            scanDir( di.absoluteFilePath(), depth + 1, DirLister::Recursive );
        else //should be the non-recursive case since the second test above should only happen with a new dir
            scanDir( di.absoluteFilePath(), depth + 1, DirLister::MTimeOnly );
    }
}


MusicScanner::MusicScanner( const QStringList& dirs, bool recursive, quint32 bs )
    : QObject()
    , m_dirs( dirs )
    , m_recursive( recursive )
    , m_batchsize( bs )
    , m_dirLister( 0 )
    , m_dirListerThreadController( 0 )
{
    m_ext2mime.insert( "mp3", TomahawkUtils::extensionToMimetype( "mp3" ) );

#ifndef NO_OGG
    m_ext2mime.insert( "ogg", TomahawkUtils::extensionToMimetype( "ogg" ) );
#endif
#ifndef NO_FLAC
    m_ext2mime.insert( "flac", TomahawkUtils::extensionToMimetype( "flac" ) );
#endif
}


MusicScanner::~MusicScanner()
{
    qDebug() << Q_FUNC_INFO;

    if( m_dirListerThreadController )
    {
        m_dirListerThreadController->quit();

        while( !m_dirListerThreadController->isFinished() )
        {
            QCoreApplication::processEvents( QEventLoop::AllEvents, 200 );
            TomahawkUtils::Sleep::msleep( 100 );
        }

        if( m_dirLister )
        {
            delete m_dirLister;
            m_dirLister = 0;
        }
        
        delete m_dirListerThreadController;
        m_dirListerThreadController = 0;
    }
}


void
MusicScanner::startScan()
{
    qDebug() << "Loading mtimes...";
    m_scanned = m_skipped = 0;
    m_skippedFiles.clear();

    // trigger the scan once we've loaded old mtimes for dirs below our path
    //FIXME: MULTIPLECOLLECTIONDIRS
    DatabaseCommand_DirMtimes* cmd = new DatabaseCommand_DirMtimes( m_dirs.first() );
    connect( cmd, SIGNAL( done( QMap<QString, unsigned int> ) ),
                    SLOT( setMtimes( QMap<QString, unsigned int> ) ) );
    connect( cmd, SIGNAL( done( QMap<QString, unsigned int> ) ),
                    SLOT( scan() ) );

    Database::instance()->enqueue( QSharedPointer<DatabaseCommand>(cmd) );
}


void
MusicScanner::setMtimes( const QMap<QString, unsigned int>& m )
{
    m_dirmtimes = m;
}


void
MusicScanner::scan()
{
    qDebug() << "Scanning, num saved mtimes from last scan:" << m_dirmtimes.size();

    connect( this, SIGNAL( batchReady( QVariantList ) ),
                     SLOT( commitBatch( QVariantList ) ), Qt::DirectConnection );

    m_dirListerThreadController = new QThread( this );
    
    m_dirLister = new DirLister( m_dirs, m_dirmtimes, m_recursive );
    m_dirLister->moveToThread( m_dirListerThreadController );

    connect( m_dirLister, SIGNAL( fileToScan( QFileInfo ) ),
                            SLOT( scanFile( QFileInfo ) ), Qt::QueuedConnection );

    // queued, so will only fire after all dirs have been scanned:
    connect( m_dirLister, SIGNAL( finished( QMap<QString, unsigned int> ) ),
                            SLOT( listerFinished( QMap<QString, unsigned int> ) ), Qt::QueuedConnection );
    
    m_dirListerThreadController->start();
    QMetaObject::invokeMethod( m_dirLister, "go" );
}


void
MusicScanner::listerFinished( const QMap<QString, unsigned int>& newmtimes )
{
    qDebug() << Q_FUNC_INFO;

    // any remaining stuff that wasnt emitted as a batch:
    if( m_scannedfiles.length() )
    {
        SourceList::instance()->getLocal()->scanningFinished( m_scanned );
        commitBatch( m_scannedfiles );
    }

    // remove obsolete / stale files
    foreach ( const QString& path, m_dirmtimes.keys() )
    {
        if ( !newmtimes.keys().contains( path ) )
        {
            qDebug() << "Removing stale dir:" << path;
            Database::instance()->enqueue( QSharedPointer<DatabaseCommand>( new DatabaseCommand_DeleteFiles( path, SourceList::instance()->getLocal() ) ) );
            emit removeWatchedDir( path );
        }
    }

    emit addWatchedDirs( newmtimes.keys() );

    // save mtimes, then quit thread
    DatabaseCommand_DirMtimes* cmd = new DatabaseCommand_DirMtimes( newmtimes );
    connect( cmd, SIGNAL( finished() ), SLOT( deleteLister() ) );
    Database::instance()->enqueue( QSharedPointer<DatabaseCommand>(cmd) );

    qDebug() << "Scanning complete, saving to database. "
                "( scanned" << m_scanned << "skipped" << m_skipped << ")";

    qDebug() << "Skipped the following files (no tags / no valid audio):";
    foreach( const QString& s, m_skippedFiles )
        qDebug() << s;
}


void
MusicScanner::deleteLister()
{
    qDebug() << Q_FUNC_INFO;
    connect( m_dirListerThreadController, SIGNAL( finished() ), SLOT( listerQuit() ) );
    m_dirListerThreadController->quit();
}


void
MusicScanner::listerQuit()
{
    qDebug() << Q_FUNC_INFO;
    connect( m_dirLister, SIGNAL( destroyed( QObject* ) ), SLOT( listerDestroyed( QObject* ) ) );
    delete m_dirLister;
    m_dirLister = 0;
}


void
MusicScanner::listerDestroyed( QObject* dirLister )
{
    qDebug() << Q_FUNC_INFO;
    m_dirListerThreadController->deleteLater();
    m_dirListerThreadController = 0;
    emit finished();
}


void
MusicScanner::commitBatch( const QVariantList& tracks )
{
    if ( tracks.length() )
    {
        qDebug() << Q_FUNC_INFO << tracks.length();
        source_ptr localsrc = SourceList::instance()->getLocal();
        Database::instance()->enqueue(
            QSharedPointer<DatabaseCommand>( new DatabaseCommand_AddFiles( tracks, localsrc ) )
        );
    }
}


void
MusicScanner::scanFile( const QFileInfo& fi )
{
    QVariant m = readFile( fi );
    if ( m.toMap().isEmpty() )
        return;

    m_scannedfiles << m;
    if ( m_batchsize != 0 && (quint32)m_scannedfiles.length() >= m_batchsize )
    {
        qDebug() << "batchReady, size:" << m_scannedfiles.length();
        emit batchReady( m_scannedfiles );
        m_scannedfiles.clear();
    }
}


QVariant
MusicScanner::readFile( const QFileInfo& fi )
{
    const QString suffix = fi.suffix().toLower();

    if ( ! m_ext2mime.contains( suffix ) )
    {
        m_skipped++;
        return QVariantMap(); // invalid extension
    }

    if ( m_scanned )
        if( m_scanned % 3 == 0 )
            SourceList::instance()->getLocal()->scanningProgress( m_scanned );
    if( m_scanned % 100 == 0 )
        qDebug() << "SCAN" << m_scanned << fi.absoluteFilePath();

    #ifdef COMPLEX_TAGLIB_FILENAME
        const wchar_t *encodedName = reinterpret_cast< const wchar_t * >( fi.absoluteFilePath().utf16() );
    #else
        QByteArray fileName = QFile::encodeName( fi.absoluteFilePath() );
        const char *encodedName = fileName.constData();
    #endif

    TagLib::FileRef f( encodedName );
    if ( f.isNull() || !f.tag() )
    {
        // qDebug() << "Doesn't seem to be a valid audiofile:" << fi.absoluteFilePath();
        m_skippedFiles << fi.absoluteFilePath();
        m_skipped++;
        return QVariantMap();
    }

    int bitrate = 0;
    int duration = 0;
    TagLib::Tag *tag = f.tag();
    if ( f.audioProperties() )
    {
        TagLib::AudioProperties *properties = f.audioProperties();
        duration = properties->length();
        bitrate = properties->bitrate();
    }

    QString artist = TStringToQString( tag->artist() ).trimmed();
    QString album  = TStringToQString( tag->album() ).trimmed();
    QString track  = TStringToQString( tag->title() ).trimmed();
    if ( artist.isEmpty() || track.isEmpty() )
    {
        // FIXME: do some clever filename guessing
        // qDebug() << "No tags found, skipping" << fi.absoluteFilePath();
        m_skippedFiles << fi.absoluteFilePath();
        m_skipped++;
        return QVariantMap();
    }

    QString mimetype = m_ext2mime.value( suffix );
    QString url( "file://%1" );

    QVariantMap m;
    m["url"]          = url.arg( fi.absoluteFilePath() );
    m["mtime"]        = fi.lastModified().toUTC().toTime_t();
    m["size"]         = (unsigned int)fi.size();
    m["mimetype"]     = mimetype;
    m["duration"]     = duration;
    m["bitrate"]      = bitrate;
    m["artist"]       = artist;
    m["album"]        = album;
    m["track"]        = track;
    m["albumpos"]     = tag->track();
    m["year"]         = tag->year();
    m["hash"]         = ""; // TODO
    
    m_scanned++;
    return m;
}
