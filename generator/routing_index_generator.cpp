#include "generator/routing_index_generator.hpp"

#include "generator/borders_generator.hpp"
#include "generator/borders_loader.hpp"

#include "routing/base/astar_algorithm.hpp"
#include "routing/cross_mwm_connector.hpp"
#include "routing/cross_mwm_connector_serialization.hpp"
#include "routing/index_graph.hpp"
#include "routing/index_graph_loader.hpp"
#include "routing/index_graph_serialization.hpp"
#include "routing/vehicle_mask.hpp"

#include "routing_common/bicycle_model.hpp"
#include "routing_common/car_model.hpp"
#include "routing_common/pedestrian_model.hpp"

#include "indexer/coding_params.hpp"
#include "indexer/data_header.hpp"
#include "indexer/feature.hpp"
#include "indexer/feature_processor.hpp"
#include "indexer/point_to_int64.hpp"

#include "coding/file_container.hpp"
#include "coding/file_name_utils.hpp"

#include "base/checked_cast.hpp"
#include "base/logging.hpp"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace feature;
using namespace platform;
using namespace routing;
using namespace std;

namespace
{
class VehicleMaskBuilder final
{
public:
  explicit VehicleMaskBuilder(string const & country)
    : m_pedestrianModel(PedestrianModelFactory().GetVehicleModelForCountry(country))
    , m_bicycleModel(BicycleModelFactory().GetVehicleModelForCountry(country))
    , m_carModel(CarModelFactory().GetVehicleModelForCountry(country))
  {
    CHECK(m_pedestrianModel, ());
    CHECK(m_bicycleModel, ());
    CHECK(m_carModel, ());
  }

  VehicleMask CalcRoadMask(FeatureType const & f) const
  {
    return CalcMask(
        f, [&](IVehicleModel const & model, FeatureType const & f) { return model.IsRoad(f); });
  }

  VehicleMask CalcOneWayMask(FeatureType const & f) const
  {
    return CalcMask(
        f, [&](IVehicleModel const & model, FeatureType const & f) { return model.IsOneWay(f); });
  }

private:
  template <class Fn>
  VehicleMask CalcMask(FeatureType const & f, Fn && fn) const
  {
    VehicleMask mask = 0;
    if (fn(*m_pedestrianModel, f))
      mask |= kPedestrianMask;
    if (fn(*m_bicycleModel, f))
      mask |= kBicycleMask;
    if (fn(*m_carModel, f))
      mask |= kCarMask;

    return mask;
  }

  shared_ptr<IVehicleModel> const m_pedestrianModel;
  shared_ptr<IVehicleModel> const m_bicycleModel;
  shared_ptr<IVehicleModel> const m_carModel;
};

class Processor final
{
public:
  explicit Processor(string const & country) : m_maskBuilder(country) {}

  void ProcessAllFeatures(string const & filename)
  {
    feature::ForEachFromDat(filename, bind(&Processor::ProcessFeature, this, _1, _2));
  }

  void BuildGraph(IndexGraph & graph) const
  {
    vector<Joint> joints;
    for (auto const & it : m_posToJoint)
    {
      // Need only connected points (2 or more roads)
      if (it.second.GetSize() >= 2)
        joints.emplace_back(it.second);
    }

    graph.Import(joints);
  }

  unordered_map<uint32_t, VehicleMask> const & GetMasks() const { return m_masks; }

private:
  void ProcessFeature(FeatureType const & f, uint32_t id)
  {
    VehicleMask const mask = m_maskBuilder.CalcRoadMask(f);
    if (mask == 0)
      return;

    m_masks[id] = mask;
    f.ParseGeometry(FeatureType::BEST_GEOMETRY);

    for (size_t i = 0; i < f.GetPointsCount(); ++i)
    {
      uint64_t const locationKey = PointToInt64(f.GetPoint(i), POINT_COORD_BITS);
      m_posToJoint[locationKey].AddPoint(RoadPoint(id, base::checked_cast<uint32_t>(i)));
    }
  }

  VehicleMaskBuilder const m_maskBuilder;
  unordered_map<uint64_t, Joint> m_posToJoint;
  unordered_map<uint32_t, VehicleMask> m_masks;
};

class DijkstraWrapper final
{
public:
  // AStarAlgorithm types aliases:
  using TVertexType = Segment;
  using TEdgeType = SegmentEdge;

  explicit DijkstraWrapper(IndexGraph & graph) : m_graph(graph) {}

  void GetOutgoingEdgesList(TVertexType const & vertex, vector<TEdgeType> & edges)
  {
    edges.clear();
    m_graph.GetEdgeList(vertex, true /* isOutgoing */, edges);
  }

  void GetIngoingEdgesList(TVertexType const & vertex, vector<TEdgeType> & edges)
  {
    edges.clear();
    m_graph.GetEdgeList(vertex, false /* isOutgoing */, edges);
  }

  double HeuristicCostEstimate(TVertexType const & /* from */, TVertexType const & /* to */)
  {
    return 0.0;
  }

private:
  IndexGraph & m_graph;
};

bool RegionsContain(vector<m2::RegionD> const & regions, m2::PointD const & point)
{
  for (auto const & region : regions)
  {
    if (region.Contains(point))
      return true;
  }

  return false;
}

void CalcCrossMwmTransitions(string const & path, string const & mwmFile, string const & country,
                             vector<CrossMwmConnectorSerializer::Transition> & transitions,
                             CrossMwmConnectorPerVehicleType & connectors)
{
  string const polyFile = my::JoinPath(path, BORDERS_DIR, country + BORDERS_EXTENSION);
  vector<m2::RegionD> borders;
  osm::LoadBorders(polyFile, borders);

  VehicleMaskBuilder const maskMaker(country);

  feature::ForEachFromDat(mwmFile, [&](FeatureType const & f, uint32_t featureId) {
    VehicleMask const roadMask = maskMaker.CalcRoadMask(f);
    if (roadMask == 0)
      return;

    f.ParseGeometry(FeatureType::BEST_GEOMETRY);
    size_t const pointsCount = f.GetPointsCount();
    if (pointsCount == 0)
      return;

    bool prevPointIn = RegionsContain(borders, f.GetPoint(0));

    for (size_t i = 1; i < pointsCount; ++i)
    {
      bool const currPointIn = RegionsContain(borders, f.GetPoint(i));
      if (currPointIn == prevPointIn)
        continue;

      uint32_t const segmentIdx = base::asserted_cast<uint32_t>(i - 1);
      VehicleMask const oneWayMask = maskMaker.CalcOneWayMask(f);

      transitions.emplace_back(featureId, segmentIdx, roadMask, oneWayMask, currPointIn,
                               f.GetPoint(i - 1), f.GetPoint(i));

      for (size_t j = 0; j < connectors.size(); ++j)
      {
        VehicleMask const mask = GetVehicleMask(static_cast<VehicleType>(j));
        CrossMwmConnectorSerializer::AddTransition(transitions.back(), mask, connectors[j]);
      }

      prevPointIn = currPointIn;
    }
  });
}

void FillWeights(string const & path, string const & mwmFile, string const & country,
                 CrossMwmConnector & connector)
{
  shared_ptr<IVehicleModel> vehicleModel = CarModelFactory().GetVehicleModelForCountry(country);
  IndexGraph graph(
      GeometryLoader::CreateFromFile(mwmFile, vehicleModel),
      EdgeEstimator::CreateForCar(nullptr /* trafficStash */, vehicleModel->GetMaxSpeed()));

  MwmValue mwmValue(LocalCountryFile(path, platform::CountryFile(country), 0 /* version */));
  DeserializeIndexGraph(mwmValue, graph);

  map<Segment, map<Segment, double>> weights;
  map<Segment, double> distanceMap;
  auto const numEnters = connector.GetEnters().size();
  for (size_t i = 0; i < numEnters; ++i)
  {
    if ((i % 10 == 0) && (i != 0))
      LOG(LINFO, ("Building leaps:", i, "/", numEnters, "waves passed"));

    Segment const & enter = connector.GetEnter(i);

    AStarAlgorithm<DijkstraWrapper> astar;
    DijkstraWrapper wrapper(graph);
    astar.PropagateWave(
        wrapper, enter, [](Segment const & /* vertex */) { return false; },
        [](Segment const & /* vertex */, SegmentEdge const & edge) { return edge.GetWeight(); },
        [](Segment const & /* from */, Segment const & /* to */) {} /* putToParents */,
        distanceMap);

    for (Segment const & exit : connector.GetExits())
    {
      auto it = distanceMap.find(exit);
      if (it != distanceMap.end())
        weights[enter][exit] = it->second;
    }
  }

  connector.FillWeights([&](Segment const & enter, Segment const & exit) {
    auto it0 = weights.find(enter);
    if (it0 == weights.end())
      return CrossMwmConnector::kNoRoute;

    auto it1 = it0->second.find(exit);
    if (it1 == it0->second.end())
      return CrossMwmConnector::kNoRoute;

    return it1->second;
  });
}

serial::CodingParams LoadCodingParams(string const & mwmFile)
{
  DataHeader const dataHeader(mwmFile);
  return dataHeader.GetDefCodingParams();
}
}  // namespace

namespace routing
{
bool BuildRoutingIndex(string const & filename, string const & country)
{
  LOG(LINFO, ("Building routing index for", filename));
  try
  {
    Processor processor(country);
    processor.ProcessAllFeatures(filename);

    IndexGraph graph;
    processor.BuildGraph(graph);

    FilesContainerW cont(filename, FileWriter::OP_WRITE_EXISTING);
    FileWriter writer = cont.GetWriter(ROUTING_FILE_TAG);

    auto const startPos = writer.Pos();
    IndexGraphSerializer::Serialize(graph, processor.GetMasks(), writer);
    auto const sectionSize = writer.Pos() - startPos;

    LOG(LINFO, ("Routing section created:", sectionSize, "bytes,", graph.GetNumRoads(), "roads,",
                graph.GetNumJoints(), "joints,", graph.GetNumPoints(), "points"));
    return true;
  }
  catch (RootException const & e)
  {
    LOG(LERROR, ("An exception happened while creating", ROUTING_FILE_TAG, "section:", e.what()));
    return false;
  }
}

void BuildCrossMwmSection(string const & path, string const & mwmFile, string const & country)
{
  LOG(LINFO, ("Building cross mwm section for", country));
  my::Timer timer;

  CrossMwmConnectorPerVehicleType connectors;

  vector<CrossMwmConnectorSerializer::Transition> transitions;
  CalcCrossMwmTransitions(path, mwmFile, country, transitions, connectors);

  LOG(LINFO, ("Transitions finished, transitions:", transitions.size(), ", elapsed:",
              timer.ElapsedSeconds(), "seconds"));
  for (size_t i = 0; i < connectors.size(); ++i)
  {
    VehicleType const vehicleType = static_cast<VehicleType>(i);
    CrossMwmConnector const & connector = connectors[i];
    LOG(LINFO, (vehicleType, "model: enters:", connector.GetEnters().size(), ", exits:",
                connector.GetExits().size()));
  }
  timer.Reset();

  FillWeights(path, mwmFile, country, connectors[static_cast<size_t>(VehicleType::Car)]);
  LOG(LINFO, ("Leaps finished, elapsed:", timer.ElapsedSeconds(), "seconds"));

  serial::CodingParams const codingParams = LoadCodingParams(mwmFile);
  FilesContainerW cont(mwmFile, FileWriter::OP_WRITE_EXISTING);
  FileWriter writer = cont.GetWriter(CROSS_MWM_FILE_TAG);
  auto const startPos = writer.Pos();
  CrossMwmConnectorSerializer::Serialize(transitions, connectors, codingParams, writer);
  auto const sectionSize = writer.Pos() - startPos;

  LOG(LINFO, ("Cross mwm section generated, size:", sectionSize, "bytes"));
}
}  // namespace routing
