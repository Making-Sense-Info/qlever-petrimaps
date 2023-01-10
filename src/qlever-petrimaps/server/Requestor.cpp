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
  _objects = _cache->getRelObjects(reader.ids);
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
    for (size_t i = batch * t; i < batch * (t+1) && i < _objects.size(); i++) {
      auto geomId = _objects[i].first;

      if (geomId < I_OFFSET) {
        auto pId = geomId;
        pointBoxes[t] =
            util::geo::extendBox(_cache->getPoints()[pId], pointBoxes[t]);
      } else if (geomId < std::numeric_limits<ID_TYPE>::max()) {
        auto lId = geomId - I_OFFSET;

        auto a = _cache->getLineBBox(lId);

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

            uint8_t sX = (curP.getX() -
                          _lpgrid.getBBox().getLowerLeft().getX() +
                          cellX * _lpgrid.getCellWidth()) / 256;
            uint8_t sY = (curP.getY() -
                          _lpgrid.getBBox().getLowerLeft().getY() +
                          cellY * _lpgrid.getCellHeight()) / 256;

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

  if (ePtr) std::rethrow_exception(ePtr);

  _ready = true;

  LOG(INFO) << "[REQUESTOR] ...done";
}

// _____________________________________________________________________________
std::vector<std::pair<std::string, std::string>> Requestor::requestRow(
    uint64_t row) const {
  RequestReader reader(_cache->getBackendURL(), _maxMemory);
  LOG(INFO) << "[REQUESTOR] Requesting single row " << row << " for query "
            << _query;
  auto query = prepQueryRow(_query, row);

  reader.requestRows(query);

  return reader.cols;
}

// _____________________________________________________________________________
std::string Requestor::prepQuery(std::string query) const {
  // only use last column
  std::regex expr("select\\s*(\\?[A-Z0-9_\\-+]*\\s*)+\\s*where\\s*\\{",
                  std::regex_constants::icase);
  query = std::regex_replace(query, expr, "SELECT $1 WHERE {");

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
  auto box = pad(getBoundingBox(rp), rad);

  size_t nearest = 0;
  double dBest = std::numeric_limits<double>::max();
  size_t nearestL = 0;
  double dBestL = std::numeric_limits<double>::max();
#pragma omp parallel sections
  {
#pragma omp section
    {
      // points

      std::unordered_set<ID_TYPE> ret;
      _pgrid.get(box, &ret);

      for (const auto& i : ret) {
        auto p = _cache->getPoints()[_objects[i].first];
        if (!util::geo::contains(p, box)) continue;

        double d = util::geo::dist(p, rp);

        if (d < dBest) {
          nearest = i;
          dBest = d;
        }
      }
    }

#pragma omp section
    {
      // lines
      std::unordered_set<ID_TYPE> retL;
      _lgrid.get(box, &retL);

      for (const auto& i : retL) {
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

        if (d < dBestL) {
          nearestL = i;
          dBestL = d;
        }
      }
    }
  }

  if (dBest < rad && dBest <= dBestL) {
    return {true, _cache->getPoints()[_objects[nearest].first],
            requestRow(_objects[nearest].second)};
  }

  if (dBestL < rad && dBestL <= dBest) {
    util::geo::FLine fline;
    size_t lineId = _objects[nearestL].first - I_OFFSET;
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

    return {true, util::geo::PolyLine<float>(fline).projectOn(rp).p,
            requestRow(_objects[nearestL].second)};
  }

  return {false, {0, 0}, {}};
}
