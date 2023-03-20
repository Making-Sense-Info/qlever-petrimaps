// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <cstring>
#include <iostream>
#include <parallel/algorithm>
#include <regex>
#include <sstream>

#include "qlever-petrimaps/Misc.h"
#include "qlever-petrimaps/server/Requestor.h"
#include "util/Misc.h"
#include "util/geo/Geo.h"
#include "util/geo/PolyLine.h"
#include "util/log/Log.h"

using petrimaps::GeomCache;
using petrimaps::Requestor;
using petrimaps::RequestReader;
using petrimaps::ResObj;

// _____________________________________________________________________________
void Requestor::request(const std::string& qry) {
  std::lock_guard<std::mutex> guard(_m);

  if (_ready) {
    // nothing to do
    return;
  }

  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }

  _query = qry;
  _ready = false;
  _objects.clear();

  RequestReader reader(_cache->getBackendURL(), _maxMemory);
  _query = qry;

  LOG(INFO) << "[REQUESTOR] Requesting IDs for query " << qry;
  reader.requestIds(prepQuery(qry));

  LOG(INFO) << "[REQUESTOR] Done, have " << reader.ids.size()
            << " ids in total.";

  // join with geoms from GeomCache

  // sort by qlever id
  LOG(INFO) << "[REQUESTOR] Sorting results by qlever ID...";
  __gnu_parallel::sort(reader.ids.begin(), reader.ids.end());
  LOG(INFO) << "[REQUESTOR] ... done";

  LOG(INFO) << "[REQUESTOR] Retrieving geoms from cache...";
  // (geom id, result row)
  const auto& ret = _cache->getRelObjects(reader.ids);
  _objects = ret.first;
  _numObjects = ret.second;
  LOG(INFO) << "[REQUESTOR] ... done, got " << _objects.size() << " objects.";

  LOG(INFO) << "[REQUESTOR] Calculating bounding box of result...";

  size_t NUM_THREADS = std::thread::hardware_concurrency();

  std::vector<util::geo::FBox> pointBoxes(NUM_THREADS);
  std::vector<util::geo::FBox> lineBoxes(NUM_THREADS);
  std::vector<size_t> numLines(NUM_THREADS, 0);
  util::geo::FBox pointBbox, lineBbox;
  size_t numLinesAll = 0;

  size_t batch = ceil(static_cast<double>(_objects.size()) / NUM_THREADS);

#pragma omp parallel for num_threads(NUM_THREADS) schedule(static)
  for (size_t t = 0; t < NUM_THREADS; t++) {
    for (size_t i = batch * t; i < batch * (t + 1) && i < _objects.size();
         i++) {
      auto geomId = _objects[i].first;

      if (geomId < I_OFFSET) {
        auto pId = geomId;
        pointBoxes[t] =
            util::geo::extendBox(_cache->getPoints()[pId], pointBoxes[t]);
      } else if (geomId < std::numeric_limits<ID_TYPE>::max()) {
        auto lId = geomId - I_OFFSET;

        lineBoxes[t] =
            util::geo::extendBox(_cache->getLineBBox(lId), lineBoxes[t]);
        numLines[t]++;
      }
    }
  }

  for (const auto& box : pointBoxes) {
    pointBbox = util::geo::extendBox(box, pointBbox);
  }

  for (const auto& box : lineBoxes) {
    lineBbox = util::geo::extendBox(box, lineBbox);
  }

  for (size_t l : numLines) {
    numLinesAll += l;
  }

  // to avoid zero area boxes if only one point is requested
  pointBbox = util::geo::pad(pointBbox, 1);
  lineBbox = util::geo::pad(lineBbox, 1);

  LOG(INFO) << "[REQUESTOR] ... done";

  LOG(INFO) << "[REQUESTOR] Point BBox: " << util::geo::getWKT(pointBbox);
  LOG(INFO) << "[REQUESTOR] Line BBox: " << util::geo::getWKT(lineBbox);
  LOG(INFO) << "[REQUESTOR] Building grid...";

  double GRID_SIZE = 65536;

  double pw =
      pointBbox.getUpperRight().getX() - pointBbox.getLowerLeft().getX();
  double ph =
      pointBbox.getUpperRight().getY() - pointBbox.getLowerLeft().getY();

  // estimate memory consumption of empty grid
  double pxWidth = fmax(0, ceil(pw / GRID_SIZE));
  double pyHeight = fmax(0, ceil(ph / GRID_SIZE));

  double lw = lineBbox.getUpperRight().getX() - lineBbox.getLowerLeft().getX();
  double lh = lineBbox.getUpperRight().getY() - lineBbox.getLowerLeft().getY();

  // estimate memory consumption of empty grid
  double lxWidth = fmax(0, ceil(lw / GRID_SIZE));
  double lyHeight = fmax(0, ceil(lh / GRID_SIZE));

  LOG(INFO) << "[REQUESTOR] (" << pxWidth << "x" << pyHeight
            << " cell point grid)";
  LOG(INFO) << "[REQUESTOR] (" << lxWidth << "x" << lyHeight
            << " cell line grid)";

  checkMem(8 * (pxWidth * pyHeight), _maxMemory);
  checkMem(8 * (lxWidth * lyHeight), _maxMemory);
  checkMem(8 * (lxWidth * lyHeight), _maxMemory);

  _pgrid = petrimaps::Grid<ID_TYPE, float>(GRID_SIZE, GRID_SIZE, pointBbox);
  _lgrid = petrimaps::Grid<ID_TYPE, float>(GRID_SIZE, GRID_SIZE, lineBbox);
  _lpgrid = petrimaps::Grid<util::geo::Point<uint8_t>, float>(
      GRID_SIZE, GRID_SIZE, lineBbox);

  std::exception_ptr ePtr;

#pragma omp parallel sections
  {
#pragma omp section
    {
      size_t i = 0;
      for (const auto& p : _objects) {
        auto geomId = p.first;
        if (geomId < I_OFFSET) {
          _pgrid.add(_cache->getPoints()[geomId], i);
        }
        i++;

        // every 100000 objects, check memory...
        if (i % 100000 == 0) {
          try {
            checkMem(1, _maxMemory);
          } catch (...) {
#pragma omp critical
            { ePtr = std::current_exception(); }
            break;
          }
        }
      }
    }

#pragma omp section
    {
      size_t i = 0;
      for (const auto& l : _objects) {
        if (l.first >= I_OFFSET &&
            l.first < std::numeric_limits<ID_TYPE>::max()) {
          auto geomId = l.first - I_OFFSET;
          _lgrid.add(_cache->getLineBBox(geomId), i);
        }
        i++;

        // every 100000 objects, check memory...
        if (i % 100000 == 0) {
          try {
            checkMem(1, _maxMemory);
          } catch (...) {
#pragma omp critical
            { ePtr = std::current_exception(); }
            break;
          }
        }
      }
    }

#pragma omp section
    {
      size_t i = 0;
      for (const auto& l : _objects) {
        if (l.first >= I_OFFSET &&
            l.first < std::numeric_limits<ID_TYPE>::max()) {
          auto geomId = l.first - I_OFFSET;

          size_t start = _cache->getLine(geomId);
          size_t end = _cache->getLineEnd(geomId);

          double mainX = 0;
          double mainY = 0;

          size_t gi = 0;

          uint8_t lastX = 0;
          uint8_t lastY = 0;

          for (size_t li = start; li < end; li++) {
            const auto& cur = _cache->getLinePoints()[li];

            if (isMCoord(cur.getX())) {
              mainX = rmCoord(cur.getX());
              mainY = rmCoord(cur.getY());
              continue;
            }

            // skip bounding box at beginning
            if (++gi < 3) continue;

            // extract real geometry
            util::geo::FPoint curP(mainX * M_COORD_GRANULARITY + cur.getX(),
                                   mainY * M_COORD_GRANULARITY + cur.getY());

            size_t cellX = _lpgrid.getCellXFromX(curP.getX());
            size_t cellY = _lpgrid.getCellYFromY(curP.getY());

            uint8_t sX =
                (curP.getX() - _lpgrid.getBBox().getLowerLeft().getX() +
                 cellX * _lpgrid.getCellWidth()) /
                256;
            uint8_t sY =
                (curP.getY() - _lpgrid.getBBox().getLowerLeft().getY() +
                 cellY * _lpgrid.getCellHeight()) /
                256;

            if (gi == 3 || lastX != sX || lastY != sY) {
              _lpgrid.add(cellX, cellY, {sX, sY});
              lastX = sX;
              lastY = sY;
            }
          }
        }
        i++;

        // every 100000 objects, check memory...
        if (i % 100000 == 0) {
          try {
            checkMem(1, _maxMemory);
          } catch (...) {
#pragma omp critical
            { ePtr = std::current_exception(); }
            break;
          }
        }
      }
    }
  }

  if (ePtr) {
    std::rethrow_exception(ePtr);
  }

  _ready = true;

  LOG(INFO) << "[REQUESTOR] ...done";
}

// _____________________________________________________________________________
std::vector<std::pair<std::string, std::string>> Requestor::requestRow(
    uint64_t row) const {
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }
  RequestReader reader(_cache->getBackendURL(), _maxMemory);
  LOG(INFO) << "[REQUESTOR] Requesting single row " << row << " for query "
            << _query;
  auto query = prepQueryRow(_query, row);

  reader.requestRows(query);

  if (reader.rows.size() == 0) return {};

  return reader.rows[0];
}

// _____________________________________________________________________________
void Requestor::requestRows(
    std::function<
        void(std::vector<std::vector<std::pair<std::string, std::string>>>)>
        cb) const {
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }
  RequestReader reader(_cache->getBackendURL(), _maxMemory);
  LOG(INFO) << "[REQUESTOR] Requesting rows for query " << _query;

  ReaderCbPair cbPair{&reader, cb};

  reader.requestRows(
      _query,
      [](void* contents, size_t size, size_t nmemb, void* ptr) {
        size_t realsize = size * nmemb;
        auto pr = static_cast<ReaderCbPair*>(ptr);
        try {
          // clear rows
          pr->reader->rows = {};
          pr->reader->parse(static_cast<const char*>(contents), realsize);
          pr->cb(pr->reader->rows);
        } catch (...) {
          pr->reader->exceptionPtr = std::current_exception();
          return static_cast<size_t>(CURLE_WRITE_ERROR);
        }

        return realsize;
      },
      &cbPair);
}

// _____________________________________________________________________________
std::string Requestor::prepQuery(std::string query) const {
  // only use last column
  std::regex expr("select\\s*(\\?[A-Z0-9_\\-+]*\\s*)+\\s*where\\s*\\{",
                  std::regex_constants::icase);

  // only remove columns the first (=outer) SELECT statement
  query = std::regex_replace(query, expr, "SELECT $1 WHERE {",
                             std::regex_constants::format_first_only);

  if (util::toLower(query).find("limit") == std::string::npos) {
    query += " LIMIT 18446744073709551615";
  }

  return query;
}

// _____________________________________________________________________________
std::string Requestor::prepQueryRow(std::string query, uint64_t row) const {
  query += " OFFSET " + std::to_string(row) + " LIMIT 1";
  return query;
}

// _____________________________________________________________________________
const ResObj Requestor::getNearest(util::geo::FPoint rp, double rad) const {
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }
  auto box = pad(getBoundingBox(rp), rad);

  size_t NUM_THREADS = std::thread::hardware_concurrency();

  size_t nearest = 0;
  double dBest = std::numeric_limits<double>::max();
  std::vector<size_t> nearestVec(NUM_THREADS, 0);
  std::vector<double> dBestVec(NUM_THREADS, std::numeric_limits<double>::max());

  std::vector<size_t> nearestLVec(NUM_THREADS, 0);
  std::vector<double> dBestLVec(NUM_THREADS,
                                std::numeric_limits<double>::max());
  size_t nearestL = 0;
  double dBestL = std::numeric_limits<double>::max();
#pragma omp parallel sections
  {
#pragma omp section
    {
      // points

      std::vector<ID_TYPE> ret;
      _pgrid.get(box, &ret);

#pragma omp parallel for num_threads(NUM_THREADS) schedule(static)
      for (size_t idx = 0; idx < ret.size(); idx++) {
        const auto& i = ret[idx];
        auto p = _cache->getPoints()[_objects[i].first];
        if (!util::geo::contains(p, box)) continue;

        double d = util::geo::dist(p, rp);

        if (d < dBestVec[omp_get_thread_num()]) {
          nearestVec[omp_get_thread_num()] = i;
          dBestVec[omp_get_thread_num()] = d;
        }
      }
    }

#pragma omp section
    {
      // lines
      std::vector<ID_TYPE> retL;
      _lgrid.get(box, &retL);

#pragma omp parallel for num_threads(NUM_THREADS) schedule(static)
      for (size_t idx = 0; idx < retL.size(); idx++) {
        const auto& i = retL[idx];
        auto lBox = _cache->getLineBBox(_objects[i].first - I_OFFSET);
        if (!util::geo::intersects(lBox, box)) continue;

        size_t start = _cache->getLine(_objects[i].first - I_OFFSET);
        size_t end = _cache->getLineEnd(_objects[i].first - I_OFFSET);

        // TODO _____________________ own function
        double d = std::numeric_limits<double>::infinity();

        util::geo::FPoint curPa, curPb;
        int s = 0;

        size_t gi = 0;

        double mainX = 0;
        double mainY = 0;

        bool isArea = Requestor::isArea(_objects[i].first - I_OFFSET);

        util::geo::FLine areaBorder;

        for (size_t i = start; i < end; i++) {
          // extract real geom
          const auto& cur = _cache->getLinePoints()[i];

          if (isMCoord(cur.getX())) {
            mainX = rmCoord(cur.getX());
            mainY = rmCoord(cur.getY());
            continue;
          }

          // skip bounding box at beginning
          gi++;
          if (gi < 3) continue;

          // extract real geometry
          util::geo::FPoint curP(mainX * M_COORD_GRANULARITY + cur.getX(),
                                 mainY * M_COORD_GRANULARITY + cur.getY());

          if (isArea) areaBorder.push_back(curP);

          if (s == 0) {
            curPa = curP;
            s++;
          } else if (s == 1) {
            curPb = curP;
            s++;
          }

          if (s == 2) {
            s = 1;
            double dTmp = util::geo::distToSegment(curPa, curPb, rp);
            if (dTmp < 0.0001) {
              d = 0;
              break;
            }
            curPa = curPb;
            if (dTmp < d) d = dTmp;
          }
        }
        // TODO _____________________ own function

        if (isArea) {
          if (util::geo::contains(rp, util::geo::FPolygon(areaBorder))) {
            // set it to rad/4 - this allows selecting smaller objects
            // inside the polgon
            d = rad / 4;
          }
        }

        if (d < dBestLVec[omp_get_thread_num()]) {
          nearestLVec[omp_get_thread_num()] = i;
          dBestLVec[omp_get_thread_num()] = d;
        }
      }
    }
  }

  // join threads
  for (size_t i = 0; i < NUM_THREADS; i++) {
    if (dBestVec[i] < dBest) {
      dBest = dBestVec[i];
      nearest = nearestVec[i];
    }

    if (dBestLVec[i] < dBestL) {
      dBestL = dBestLVec[i];
      nearestL = nearestLVec[i];
    }
  }

  if (dBest < rad && dBest <= dBestL) {
    return {true,
            nearest,
            geomPointGeoms(nearest),
            requestRow(_objects[nearest].second),
            {},
            {}};
  }

  if (dBestL < rad && dBestL <= dBest) {
    size_t lineId = _objects[nearestL].first - I_OFFSET;

    bool isArea = Requestor::isArea(lineId);

    const auto& fline = extractLineGeom(lineId);

    if (isArea && util::geo::contains(rp, util::geo::FPolygon(fline))) {
      return {true, nearestL,
              {rp}, requestRow(_objects[nearestL].second),
              {},   geomPolyGeoms(nearestL, rad / 10)};
    } else {
      if (isArea) {
        return {true,
                nearestL,
                {util::geo::PolyLine<float>(fline).projectOn(rp).p},
                requestRow(_objects[nearestL].second),
                {},
                geomPolyGeoms(nearestL, rad / 10)};
      } else {
        return {true,
                nearestL,
                {util::geo::PolyLine<float>(fline).projectOn(rp).p},
                requestRow(_objects[nearestL].second),
                geomLineGeoms(nearestL, rad / 10),
                {}};
      }
    }
  }

  return {false, 0, {{0, 0}}, {}, {}, {}};
}

// _____________________________________________________________________________
const ResObj Requestor::getGeom(size_t id, double rad) const {
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }
  auto obj = _objects[id];

  if (obj.first >= I_OFFSET) {
    size_t lineId = obj.first - I_OFFSET;

    bool isArea = Requestor::isArea(lineId);
    const auto& fline = extractLineGeom(lineId);

    if (isArea) {
      return {true, id, {{0, 0}}, {}, {}, geomPolyGeoms(id, rad / 10)};
    } else {
      return {true, id, {{0, 0}}, {}, geomLineGeoms(id, rad / 10), {}};
    }
  } else {
    return {true, id, geomPointGeoms(id), {}, {}, {}};
  }
}

// _____________________________________________________________________________
util::geo::FLine Requestor::extractLineGeom(size_t lineId) const {
  util::geo::FLine fline;

  size_t start = _cache->getLine(lineId);
  size_t end = _cache->getLineEnd(lineId);

  double mainX = 0;
  double mainY = 0;

  size_t gi = 0;

  for (size_t i = start; i < end; i++) {
    // extract real geom
    const auto& cur = _cache->getLinePoints()[i];

    if (isMCoord(cur.getX())) {
      mainX = rmCoord(cur.getX());
      mainY = rmCoord(cur.getY());
      continue;
    }

    // skip bounding box at beginning
    gi++;
    if (gi < 3) continue;

    util::geo::FPoint curP(mainX * M_COORD_GRANULARITY + cur.getX(),
                           mainY * M_COORD_GRANULARITY + cur.getY());
    fline.push_back(curP);
  }

  return fline;
}

// _____________________________________________________________________________
bool Requestor::isArea(size_t lineId) const {
  size_t end = _cache->getLineEnd(lineId);

  return isMCoord(_cache->getLinePoints()[end - 1].getX());
}

// _____________________________________________________________________________
util::geo::MultiLine<float> Requestor::geomLineGeoms(size_t oid,
                                                     double eps) const {
  std::vector<util::geo::FLine> polys;

  // catch multigeometries
  for (size_t i = oid;
       i < _objects.size() && _objects[i].second == _objects[oid].second; i++) {
    if (_objects[oid].first < I_OFFSET) continue;
    const auto& fline = extractLineGeom(_objects[i].first - I_OFFSET);
    polys.push_back(util::geo::simplify(fline, eps));
  }

  for (size_t i = oid - 1;
       i < _objects.size() && _objects[i].second == _objects[oid].second; i--) {
    if (_objects[oid].first < I_OFFSET) continue;
    const auto& fline = extractLineGeom(_objects[i].first - I_OFFSET);
    polys.push_back(util::geo::simplify(fline, eps));
  }

  return polys;
}

// _____________________________________________________________________________
util::geo::MultiPoint<float> Requestor::geomPointGeoms(size_t oid) const {
  std::vector<util::geo::FPoint> points;

  // catch multigeometries
  for (size_t i = oid;
       i < _objects.size() && _objects[i].second == _objects[oid].second; i++) {
    if (_objects[oid].first >= I_OFFSET) continue;
    points.push_back(_cache->getPoints()[_objects[i].first]);
  }

  for (size_t i = oid - 1;
       i < _objects.size() && _objects[i].second == _objects[oid].second; i--) {
    if (_objects[oid].first >= I_OFFSET) continue;
    points.push_back(_cache->getPoints()[_objects[i].first]);
  }

  return points;
}

// _____________________________________________________________________________
util::geo::MultiPolygon<float> Requestor::geomPolyGeoms(size_t oid,
                                                        double eps) const {
  std::vector<util::geo::FPolygon> polys;

  // catch multigeometries
  for (size_t i = oid;
       i < _objects.size() && _objects[i].second == _objects[oid].second; i++) {
    if (_objects[oid].first < I_OFFSET) continue;
    const auto& fline = extractLineGeom(_objects[i].first - I_OFFSET);
    polys.push_back(util::geo::FPolygon(util::geo::simplify(fline, eps)));
  }

  for (size_t i = oid - 1;
       i < _objects.size() && _objects[i].second == _objects[oid].second; i--) {
    if (_objects[oid].first < I_OFFSET) continue;
    const auto& fline = extractLineGeom(_objects[i].first - I_OFFSET);
    polys.push_back(util::geo::FPolygon(util::geo::simplify(fline, eps)));
  }

  return polys;
}
