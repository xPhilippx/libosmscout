/*
  OSMScout - a Qt backend for libosmscout and libosmscout-map
  Copyright (C) 2010  Tim Teulings
  Copyright (C) 2017  Lukáš Karas

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#include <osmscout/TiledMapRenderer.h>
#include <osmscout/OSMTile.h>

TiledMapRenderer::TiledMapRenderer(QThread *thread,
                                   SettingsRef settings,
                                   DBThreadRef dbThread,
                                   QString iconDirectory,
                                   QString tileCacheDirectory,
                                   size_t onlineTileCacheSize,
                                   size_t offlineTileCacheSize):
  MapRenderer(thread,settings,dbThread,iconDirectory),
  tileCacheDirectory(tileCacheDirectory),
  onlineTileCache(onlineTileCacheSize), // online tiles can be loaded from disk cache easily
  offlineTileCache(offlineTileCacheSize), // render offline tile is expensive
  tileDownloader(NULL) // it will be created in different thread
{
  QScreen *srn=QGuiApplication::primaryScreen();
  screenWidth=srn->availableSize().width();
  screenHeight=srn->availableSize().height();


  onlineTilesEnabled = settings->GetOnlineTilesEnabled();
  offlineTilesEnabled = settings->GetOfflineMap();

  connect(settings.get(), SIGNAL(OnlineTileProviderIdChanged(const QString)),
          this, SLOT(onlineTileProviderChanged()));
  connect(settings.get(), SIGNAL(OnlineTilesEnabledChanged(bool)),
          this, SLOT(onlineTilesEnabledChanged(bool)));
  connect(settings.get(), SIGNAL(OfflineMapChanged(bool)),
          this, SLOT(onOfflineMapChanged(bool)));

  connect(dbThread.get(), SIGNAL(databaseLoadFinished(osmscout::GeoBox)),
          this, SLOT(onDatabaseLoaded(osmscout::GeoBox)));
  //
  // Make sure that we always decouple caller and receiver even if they are running in the same thread
  // else we might get into a dead lock
  //

  connect(&onlineTileCache,SIGNAL(tileRequested(uint32_t, uint32_t, uint32_t)),
          this,SLOT(onlineTileRequest(uint32_t, uint32_t, uint32_t)),
          Qt::QueuedConnection);

  connect(&offlineTileCache,SIGNAL(tileRequested(uint32_t, uint32_t, uint32_t)),
          this,SLOT(offlineTileRequest(uint32_t, uint32_t, uint32_t)),
          Qt::QueuedConnection);

}

TiledMapRenderer::~TiledMapRenderer()
{
  if (tileDownloader != NULL){
    delete tileDownloader;
  }
}

void TiledMapRenderer::InvalidateVisualCache()
{
  // invalidate tile cache
  QMutexLocker locker(&tileCacheMutex);
  offlineTileCache.invalidate();
  offlineTileCache.clearPendingRequests();
}

void TiledMapRenderer::onStylesheetFilenameChanged(){
  // invalidate tile cache and emit Redraw
  {
      QMutexLocker locker(&tileCacheMutex);
      offlineTileCache.invalidate();
      offlineTileCache.clearPendingRequests();
  }
  emit Redraw();
}

/**
 * Render map defined by request to painter
 * @param painter
 * @param request
 * @return true if rendered map is complete
 */
bool TiledMapRenderer::RenderMap(QPainter& painter,
                                 const RenderMapRequest& request)
{
  osmscout::MercatorProjection projection;

  projection.Set(request.coord,
                 request.angle,
                 request.magnification,
                 mapDpi,
                 request.width,
                 request.height);

  osmscout::GeoBox boundingBox;

  projection.GetDimensions(boundingBox);


  QColor white = QColor::fromRgbF(1.0,1.0,1.0);
  //QColor grey = QColor::fromRgbF(0.5,0.5,0.5);
  QColor grey2 = QColor::fromRgbF(0.8,0.8,0.8);

  painter.fillRect( 0,0,
                    projection.GetWidth(),projection.GetHeight(),
                    white);

  // OpenStreetMap render its tiles up to latitude +-85.0511
  double osmMinLat = OSMTile::minLat();
  double osmMaxLat = OSMTile::maxLat();
  double osmMinLon = OSMTile::minLon();
  double osmMaxLon = OSMTile::maxLon();

  uint32_t osmTileRes = OSMTile::worldRes(projection.GetMagnification().GetLevel());
  double x1;
  double y1;
  projection.GeoToPixel(osmscout::GeoCoord(osmMaxLat, osmMinLon), x1, y1);
  double x2;
  double y2;
  projection.GeoToPixel(osmscout::GeoCoord(osmMinLat, osmMaxLon), x2, y2);

  double renderTileWidth = (x2 - x1) / osmTileRes; // pixels
  double renderTileHeight = (y2 - y1) / osmTileRes; // pixels

  painter.setPen(grey2);

  uint32_t osmTileFromX = std::max(0.0, (double)osmTileRes * ((boundingBox.GetMinLon() + (double)180.0) / (double)360.0));
  double maxLatRad = boundingBox.GetMaxLat() * GRAD_TO_RAD;
  uint32_t osmTileFromY = std::max(0.0, (double)osmTileRes * ((double)1.0 - (log(tan(maxLatRad) + (double)1.0 / cos(maxLatRad)) / M_PI)) / (double)2.0);

  //std::cout << osmTileRes << " * (("<< boundingBox.GetMinLon()<<" + 180.0) / 360.0) = " << osmTileFromX << std::endl;
  //std::cout <<  osmTileRes<<" * (1.0 - (log(tan("<<maxLatRad<<") + 1.0 / cos("<<maxLatRad<<")) / "<<M_PI<<")) / 2.0 = " << osmTileFromY << std::endl;

  uint32_t zoomLevel = projection.GetMagnification().GetLevel();

  /*
  double osmTileDimension = (double)OSMTile::osmTileOriginalWidth() * (dpi / OSMTile::tileDPI() ); // pixels
  std::cout <<
    "level: " << zoomLevel <<
    " request WxH " << request.width << " x " << request.height <<
    " osmTileRes: " << osmTileRes <<
    " scaled tile dimension: " << osmTileWidth << " x " << osmTileHeight << " (" << osmTileDimension << ")"<<
    " osmTileFromX: " << osmTileFromX << " cnt " << (projection.GetWidth() / (uint32_t)osmTileWidth) <<
    " osmTileFromY: " << osmTileFromY << " cnt " << (projection.GetHeight() / (uint32_t)osmTileHeight) <<
    " current thread : " << QThread::currentThread() <<
    std::endl;
   */

  // render available tiles
  double x;
  double y;
  QTime start;
  QMutexLocker locker(&tileCacheMutex);
  int elapsed = start.elapsed();
  if (elapsed > 1){
      std::cout << "Mutex acquiere took " << elapsed << " ms" << std::endl;
  }

  onlineTileCache.clearPendingRequests();
  offlineTileCache.clearPendingRequests();
  for ( uint32_t ty = 0;
        (ty <= (projection.GetHeight() / (uint32_t)renderTileHeight)+1) && ((osmTileFromY + ty) < osmTileRes);
        ty++ ){

    //for (uint32_t i = 1; i< osmTileRes; i++){
    uint32_t ytile = (osmTileFromY + ty);
    double ytileLatRad = atan(sinh(M_PI * (1 - 2 * (double)ytile / (double)osmTileRes)));
    double ytileLatDeg = ytileLatRad * 180.0 / M_PI;

    for ( uint32_t tx = 0;
          (tx <= (projection.GetWidth() / (uint32_t)renderTileWidth)+1) && ((osmTileFromX + tx) < osmTileRes);
          tx++ ){

      uint32_t xtile = (osmTileFromX + tx);
      double xtileDeg = (double)xtile / (double)osmTileRes * 360.0 - 180.0;

      projection.GeoToPixel(osmscout::GeoCoord(ytileLatDeg, xtileDeg), x, y);
      //double x = x1 + (double)xtile * osmTileWidth;

      //std::cout << "  xtile: " << xtile << " ytile: " << ytile << " x: " << x << " y: " << y << "" << std::endl;

      //bool lookupTileFound = false;

      bool lookupTileFound = false;
      if (onlineTilesEnabled){
        lookupTileFound |= lookupAndDrawTile(onlineTileCache, painter,
              x, y, renderTileWidth, renderTileHeight,
              zoomLevel, xtile, ytile, /* up limit */ 6, /* down limit */ 3
              );
      }

      if (offlineTilesEnabled){
        lookupTileFound |= lookupAndDrawTile(offlineTileCache, painter,
                x, y, renderTileWidth, renderTileHeight,
                zoomLevel, xtile, ytile, /* up limit */ 6, /* down limit */ 3
                );
      }

      if (!lookupTileFound){
        // no tile found, draw its outline
        painter.drawLine(x,y, x + renderTileWidth, y);
        painter.drawLine(x,y, x, y + renderTileHeight);
      }
    }
  }
  /*
  painter.setPen(grey);
  painter.drawText(20, 30, QString("%1").arg(projection.GetMagnification().GetLevel()));

  double centerLat;
  double centerLon;
  projection.PixelToGeo(projection.GetWidth() / 2.0, projection.GetHeight() / 2.0, centerLon, centerLat);
  painter.drawText(20, 60, QString::fromStdString(osmscout::GeoCoord(centerLat, centerLon).GetDisplayText()));
  */
  return onlineTileCache.isRequestQueueEmpty() && offlineTileCache.isRequestQueueEmpty();
}

bool TiledMapRenderer::lookupAndDrawTile(TileCache& tileCache, QPainter& painter,
        double x, double y, double renderTileWidth, double renderTileHeight,
        uint32_t zoomLevel, uint32_t xtile, uint32_t ytile,
        uint32_t upLimit, uint32_t downLimit)
{
    bool triggerRequest = true;

    // trick for avoiding white lines between tiles caused by antialiasing
    // http://stackoverflow.com/questions/7332118/antialiasing-leaves-thin-line-between-adjacent-widgets
    double overlap = painter.testRenderHint(QPainter::Antialiasing) ? 0.5 : 0.0;

    uint32_t lookupTileZoom = zoomLevel;
    uint32_t lookupXTile = xtile;
    uint32_t lookupYTile = ytile;
    QRectF lookupTileViewport(0, 0, 1, 1); // tile viewport (percent)
    bool lookupTileFound = false;

    // lookup upper zoom levels
    //qDebug() << "Need paint tile " << xtile << " " << ytile << " zoom " << zoomLevel;
    while ((!lookupTileFound) && (zoomLevel - lookupTileZoom <= upLimit)){
      //qDebug() << "  - lookup tile " << lookupXTile << " " << lookupYTile << " zoom " << lookupTileZoom << " " << " viewport " << lookupTileViewport;
      if (tileCache.contains(lookupTileZoom, lookupXTile, lookupYTile)){
          TileCacheVal val = tileCache.get(lookupTileZoom, lookupXTile, lookupYTile);
          if (!val.image.isNull()){
            double imageWidth = val.image.width();
            double imageHeight = val.image.height();
            QRectF imageViewport(imageWidth * lookupTileViewport.x(), imageHeight * lookupTileViewport.y(),
                    imageWidth * lookupTileViewport.width(), imageHeight * lookupTileViewport.height() );

            // TODO: support map rotation
            painter.drawPixmap(QRectF(x, y, renderTileWidth+overlap, renderTileHeight+overlap), val.image, imageViewport);
          }
          lookupTileFound = true;
          if (lookupTileZoom == zoomLevel)
              triggerRequest = false;
      }else{
          // no tile found on current zoom zoom level, lookup upper zoom level
          if (lookupTileZoom==0)
            break;
          lookupTileZoom --;
          uint32_t crop = 1 << (zoomLevel - lookupTileZoom);
          double viewportWidth = 1.0 / (double)crop;
          double viewportHeight = 1.0 / (double)crop;
          lookupTileViewport = QRectF(
                  (double)(xtile % crop) * viewportWidth,
                  (double)(ytile % crop) * viewportHeight,
                  viewportWidth,
                  viewportHeight);
          lookupXTile = lookupXTile / 2;
          lookupYTile = lookupYTile / 2;
      }
    }

    // lookup bottom zoom levels
    if (!lookupTileFound && downLimit > 0){
        lookupAndDrawBottomTileRecursive(tileCache, painter,
            x, y, renderTileWidth, renderTileHeight, overlap,
            zoomLevel, xtile, ytile,
            downLimit -1);
    }

    if (triggerRequest){
       if (tileCache.request(zoomLevel, xtile, ytile)){
         //std::cout << "  tile request: " << zoomLevel << " xtile: " << xtile << " ytile: " << ytile << std::endl;
        }else{
         //std::cout << "  requested already: " << zoomLevel << " xtile: " << xtile << " ytile: " << ytile << std::endl;
       }
    }
    return lookupTileFound;
}

void TiledMapRenderer::lookupAndDrawBottomTileRecursive(TileCache& tileCache, QPainter& painter,
        double x, double y, double renderTileWidth, double renderTileHeight, double overlap,
        uint32_t zoomLevel, uint32_t xtile, uint32_t ytile,
        uint32_t downLimit)
{
    if (zoomLevel > 20)
        return;

    //qDebug() << "Need paint tile " << xtile << " " << ytile << " zoom " << zoomLevel;
    uint32_t lookupTileZoom = zoomLevel + 1;
    uint32_t lookupXTile;
    uint32_t lookupYTile;
    uint32_t tileCnt = 2;

    for (uint32_t ty = 0; ty < tileCnt; ty++){
        lookupYTile = ytile *2 + ty;
        for (uint32_t tx = 0; tx < tileCnt; tx++){
            lookupXTile = xtile *2 + tx;
            //qDebug() << "  - lookup tile " << lookupXTile << " " << lookupYTile << " zoom " << lookupTileZoom;
            bool found = false;
            if (tileCache.contains(lookupTileZoom, lookupXTile, lookupYTile)){
                TileCacheVal val = tileCache.get(lookupTileZoom, lookupXTile, lookupYTile);
                if (!val.image.isNull()){
                    double imageWidth = val.image.width();
                    double imageHeight = val.image.height();
                    painter.drawPixmap(
                            QRectF(x + tx * (renderTileWidth/tileCnt), y + ty * (renderTileHeight/tileCnt), renderTileWidth/tileCnt + overlap, renderTileHeight/tileCnt + overlap),
                            val.image,
                            QRectF(0.0, 0.0, imageWidth, imageHeight));
                    found = true;
                }
            }
            if (!found && downLimit > 0){
                // recursion
                lookupAndDrawBottomTileRecursive(tileCache, painter,
                    x + tx * (renderTileWidth/tileCnt), y + ty * (renderTileHeight/tileCnt), renderTileWidth/tileCnt, renderTileHeight/tileCnt, overlap,
                    zoomLevel +1, lookupXTile, lookupYTile,
                    downLimit -1);
            }
        }
    }
}

DatabaseCoverage TiledMapRenderer::databaseCoverageOfTile(uint32_t zoomLevel, uint32_t xtile, uint32_t ytile)
{
  osmscout::GeoBox tileBoundingBox = OSMTile::tileBoundingBox(zoomLevel, xtile, ytile);
  osmscout::Magnification magnification;
  magnification.SetLevel(zoomLevel);
  return dbThread->databaseCoverage(magnification,tileBoundingBox);
}

void TiledMapRenderer::onDatabaseLoaded(osmscout::GeoBox boundingBox)
{
  {
    QMutexLocker locker(&tileCacheMutex);
    onlineTileCache.invalidate(boundingBox);
    offlineTileCache.invalidate(boundingBox);
  }

  emit Redraw();
}


void TiledMapRenderer::onlineTileRequest(uint32_t zoomLevel, uint32_t xtile, uint32_t ytile)
{
    {
        QMutexLocker locker(&tileCacheMutex);
        if (!onlineTileCache.startRequestProcess(zoomLevel, xtile, ytile)) // request was canceled or started already
            return;
    }

    // TODO: mutex?
    bool requestedFromWeb = onlineTilesEnabled && (!(offlineTilesEnabled &&
          databaseCoverageOfTile(zoomLevel, xtile, ytile) == DatabaseCoverage::Covered));

    if (requestedFromWeb){
        QMutexLocker locker(&lock);
        if (tileDownloader == NULL){
            qWarning() << "tile requested but donwloader is not initialized yet";
            emit tileDownloadFailed(zoomLevel, xtile, ytile, false);
        }else{
            emit tileDownloader->download(zoomLevel, xtile, ytile);
        }
    } else{
        // put Null image
        {
            QMutexLocker locker(&tileCacheMutex);
            onlineTileCache.put(zoomLevel, xtile, ytile, QImage());
        }
    }
}

void TiledMapRenderer::offlineTileRequest(uint32_t zoomLevel, uint32_t xtile, uint32_t ytile)
{
    // TODO: just start loading
    {
        QMutexLocker locker(&tileCacheMutex);
        if (!offlineTileCache.startRequestProcess(zoomLevel, xtile, ytile)) // request was canceled or started already
            return;
    }

    DatabaseCoverage state = databaseCoverageOfTile(zoomLevel, xtile, ytile);
    bool render = (state != DatabaseCoverage::Outside);
    if (render) {
        // tile rendering have sub-linear complexity with area size
        // it means that it is advatage to merge more tile requests with same zoom
        // and render bigger area
        uint32_t xFrom;
        uint32_t xTo;
        uint32_t yFrom;
        uint32_t yTo;
        {
            QMutexLocker locker(&tileCacheMutex);
            offlineTileCache.mergeAndStartRequests(zoomLevel, xtile, ytile,
                                                   xFrom, xTo, yFrom, yTo,
                                                   /*maxWidth*/ 5, /*maxHeight*/ 5);
        }
        uint32_t width = (xTo - xFrom + 1);
        uint32_t height = (yTo - yFrom + 1);

        //osmscout::GeoBox tileBoundingBox = OSMTile::tileBoundingBox(zoomLevel, xtile, ytile);
        osmscout::GeoCoord tileVisualCenter = OSMTile::tileRelativeCoord(zoomLevel,
                (double)xFrom + (double)width/2.0,
                (double)yFrom + (double)height/2.0);

        double osmTileDimension = (double)OSMTile::osmTileOriginalWidth() * (mapDpi / OSMTile::tileDPI() ); // pixels

        QImage canvas(
                (double)width * osmTileDimension,
                (double)height * osmTileDimension,
                QImage::Format_ARGB32_Premultiplied); // TODO: verify best format with profiler (callgrind)

        QColor transparent = QColor::fromRgbF(1, 1, 1, 0.0);
        canvas.fill(transparent);

        QPainter p;
        p.begin(&canvas);

        DrawMap(p, tileVisualCenter, zoomLevel, canvas.width(), canvas.height(),
                canvas.width() + osmTileDimension, canvas.height() + osmTileDimension);

        p.end();
        {
            QMutexLocker locker(&tileCacheMutex);
            if (width == 1 && height == 1){
                offlineTileCache.put(zoomLevel, xtile, ytile, canvas);
            }else{
                for (uint32_t y = yFrom; y <= yTo; ++y){
                    for (uint32_t x = xFrom; x <= xTo; ++x){

                        QImage tile = canvas.copy(
                                (double)(x - xFrom) * osmTileDimension,
                                (double)(y - yFrom) * osmTileDimension,
                                osmTileDimension, osmTileDimension
                                );

                        offlineTileCache.put(zoomLevel, x, y, tile);
                    }
                }
            }
        }
        Redraw();
        //std::cout << "  put offline: " << zoomLevel << " xtile: " << xtile << " ytile: " << ytile << std::endl;
    }else{
        // put Null image
        {
            QMutexLocker locker(&tileCacheMutex);
            offlineTileCache.put(zoomLevel, xtile, ytile, QImage());
        }
    }
}

void TiledMapRenderer::tileDownloaded(uint32_t zoomLevel, uint32_t x, uint32_t y, QImage image, QByteArray /*downloadedData*/)
{
    //QMutexLocker locker(&mutex);

    {
        QMutexLocker locker(&tileCacheMutex);
        onlineTileCache.put(zoomLevel, x, y, image);
    }
    //std::cout << "  put: " << zoomLevel << " xtile: " << x << " ytile: " << y << std::endl;

    emit Redraw();
}

void TiledMapRenderer::tileDownloadFailed(uint32_t zoomLevel, uint32_t x, uint32_t y, bool zoomLevelOutOfRange)
{
    QMutexLocker locker(&tileCacheMutex);
    onlineTileCache.removeRequest(zoomLevel, x, y);

    if (zoomLevelOutOfRange && zoomLevel > 0){
        // hack: when zoom level is too high for online source,
        // we try to request tile with lower zoom level and put it to cache
        // as substitute
        uint32_t reqZoom = zoomLevel - 1;
        uint32_t reqX = x / 2;
        uint32_t reqY = y / 2;
        if ((!onlineTileCache.contains(reqZoom, reqX, reqY))
             && onlineTileCache.request(reqZoom, reqX, reqY)){
            qDebug() << "Tile download failed " << x << " " << y << " zoomLevel " << zoomLevel << " try lower zoom";
            //triggerTileRequest(reqZoom, reqX, reqY);
        }
    }
}

void TiledMapRenderer::onlineTileProviderChanged()
{
    {
        QMutexLocker locker(&tileCacheMutex);
        onlineTileCache.invalidate();
    }
    emit Redraw();
}

void TiledMapRenderer::onlineTilesEnabledChanged(bool b)
{
    {
        QMutexLocker locker(&lock);
        onlineTilesEnabled = b;

        QMutexLocker cacheLocker(&tileCacheMutex);
        onlineTileCache.invalidate();
        onlineTileCache.clearPendingRequests();
    }
    emit Redraw();
}

void TiledMapRenderer::onOfflineMapChanged(bool b)
{
    {
        QMutexLocker locker(&lock);
        offlineTilesEnabled = b;

        QMutexLocker cacheLocker(&tileCacheMutex);
        onlineTileCache.invalidate(); // overlapp areas will change
        offlineTileCache.invalidate();
        offlineTileCache.clearPendingRequests();
    }
    emit Redraw();
}

/**
 * Actual map drawing into the back buffer
 *
 * have to be called with acquiered mutex
 */
void TiledMapRenderer::DrawMap(QPainter &p, const osmscout::GeoCoord center, uint32_t z,
        size_t width, size_t height, size_t lookupWidth, size_t lookupHeight)
{
    QMutexLocker locker(&lock);

    // TODO
}
