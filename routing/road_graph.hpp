#pragma once

#include "geometry/point2d.hpp"

#include "base/string_utils.hpp"

#include "indexer/feature_altitude.hpp"
#include "indexer/feature_data.hpp"

#include "std/initializer_list.hpp"
#include "std/map.hpp"
#include "std/vector.hpp"

namespace routing
{
double constexpr kPointsEqualEpsilon = 1e-6;

/// The Junction class represents a node description on a road network graph
class Junction
{
public:
  Junction();
  Junction(m2::PointD const & point, feature::TAltitude altitude);
  Junction(Junction const &) = default;
  Junction & operator=(Junction const &) = default;

  inline bool operator==(Junction const & r) const { return m_point == r.m_point; }
  inline bool operator!=(Junction const & r) const { return !(*this == r); }
  inline bool operator<(Junction const & r) const { return m_point < r.m_point; }

  inline m2::PointD const & GetPoint() const { return m_point; }
  inline feature::TAltitude GetAltitude() const { return m_altitude; }

private:
  friend string DebugPrint(Junction const & r);

  // Point of the junction
  m2::PointD m_point;
  feature::TAltitude m_altitude;
};

inline Junction MakeJunctionForTesting(m2::PointD const & point)
{
  return Junction(point, feature::kDefaultAltitudeMeters);
}

inline bool AlmostEqualAbs(Junction const & lhs, Junction const & rhs)
{
  return my::AlmostEqualAbs(lhs.GetPoint(), rhs.GetPoint(), kPointsEqualEpsilon);
}

/// The Edge class represents an edge description on a road network graph
class Edge
{
public:
  static Edge MakeFake(Junction const & startJunction, Junction const & endJunction,
                       bool partOfReal);

  Edge();
  Edge(FeatureID const & featureId, bool forward, uint32_t segId, Junction const & startJunction,
       Junction const & endJunction);
  Edge(Edge const &) = default;
  Edge & operator=(Edge const &) = default;

  inline FeatureID GetFeatureId() const { return m_featureId; }
  inline bool IsForward() const { return m_forward; }
  inline uint32_t GetSegId() const { return m_segId; }
  inline Junction const & GetStartJunction() const { return m_startJunction; }
  inline Junction const & GetEndJunction() const { return m_endJunction; }
  inline bool IsFake() const { return !m_featureId.IsValid(); }
  inline bool IsPartOfReal() const { return m_partOfReal; }

  Edge GetReverseEdge() const;

  bool SameRoadSegmentAndDirection(Edge const & r) const;

  bool operator==(Edge const & r) const;
  bool operator<(Edge const & r) const;

private:
  friend string DebugPrint(Edge const & r);

  // Feature for which edge is defined.
  FeatureID m_featureId;

  // Is the feature along the road.
  bool m_forward;

  // This flag is set for edges that are parts of some real edges.
  bool m_partOfReal;

  // Ordinal number of the segment on the road.
  uint32_t m_segId;

  // Start junction of the segment on the road.
  Junction m_startJunction;

  // End junction of the segment on the road.
  Junction m_endJunction;
};

class RoadGraphBase
{
public:
  typedef vector<Edge> TEdgeVector;

  /// Finds all nearest outgoing edges, that route to the junction.
  virtual void GetOutgoingEdges(Junction const & junction, TEdgeVector & edges) const = 0;

  /// Finds all nearest ingoing edges, that route to the junction.
  virtual void GetIngoingEdges(Junction const & junction, TEdgeVector & edges) const = 0;

  /// Returns max speed in KM/H
  virtual double GetMaxSpeedKMPH() const = 0;

  /// @return Types for the specified edge
  virtual void GetEdgeTypes(Edge const & edge, feature::TypesHolder & types) const = 0;

  /// @return Types for specified junction
  virtual void GetJunctionTypes(Junction const & junction, feature::TypesHolder & types) const = 0;
};

class IRoadGraph : public RoadGraphBase
{
public:
  typedef vector<Junction> TJunctionVector;

  enum class Mode
  {
    ObeyOnewayTag,
    IgnoreOnewayTag,
  };

  /// This struct contains the part of a feature's metadata that is
  /// relevant for routing.
  struct RoadInfo
  {
    RoadInfo();
    RoadInfo(RoadInfo && ri);
    RoadInfo(bool bidirectional, double speedKMPH, initializer_list<Junction> const & points);
    RoadInfo(RoadInfo const &) = default;
    RoadInfo & operator=(RoadInfo const &) = default;

    buffer_vector<Junction, 32> m_junctions;
    double m_speedKMPH;
    bool m_bidirectional;
  };

  /// This class is responsible for loading edges in a cross.
  class ICrossEdgesLoader
  {
  public:
    ICrossEdgesLoader(Junction const & cross, IRoadGraph::Mode mode, TEdgeVector & edges)
      : m_cross(cross), m_mode(mode), m_edges(edges)
    {
    }

    virtual ~ICrossEdgesLoader() = default;

    void operator()(FeatureID const & featureId, RoadInfo const & roadInfo)
    {
      LoadEdges(featureId, roadInfo);
    }

  private:
    virtual void LoadEdges(FeatureID const & featureId, RoadInfo const & roadInfo) = 0;

  protected:
    template <typename TFn>
    void ForEachEdge(RoadInfo const & roadInfo, TFn && fn)
    {
      for (size_t i = 0; i < roadInfo.m_junctions.size(); ++i)
      {
        if (!my::AlmostEqualAbs(m_cross.GetPoint(), roadInfo.m_junctions[i].GetPoint(),
                                kPointsEqualEpsilon))
        {
          continue;
        }

        if (i + 1 < roadInfo.m_junctions.size())
        {
          // Head of the edge.
          // m_cross
          //     o------------>o
          fn(i, roadInfo.m_junctions[i + 1], true /* forward */);
        }
        if (i > 0)
        {
          // Tail of the edge.
          //                m_cross
          //     o------------>o
          fn(i - 1, roadInfo.m_junctions[i - 1], false /* backward */);
        }
      }
    }

    Junction const m_cross;
    IRoadGraph::Mode const m_mode;
    TEdgeVector & m_edges;
  };

  class CrossOutgoingLoader : public ICrossEdgesLoader
  {
  public:
    CrossOutgoingLoader(Junction const & cross, IRoadGraph::Mode mode, TEdgeVector & edges)
      : ICrossEdgesLoader(cross, mode, edges)
    {
    }

  private:
    // ICrossEdgesLoader overrides:
    virtual void LoadEdges(FeatureID const & featureId, RoadInfo const & roadInfo) override;
  };

  class CrossIngoingLoader : public ICrossEdgesLoader
  {
  public:
    CrossIngoingLoader(Junction const & cross, IRoadGraph::Mode mode, TEdgeVector & edges)
      : ICrossEdgesLoader(cross, mode, edges)
    {
    }

  private:
    // ICrossEdgesLoader overrides:
    virtual void LoadEdges(FeatureID const & featureId, RoadInfo const & roadInfo) override;
  };

  virtual ~IRoadGraph() = default;

  virtual void GetOutgoingEdges(Junction const & junction, TEdgeVector & edges) const override;

  virtual void GetIngoingEdges(Junction const & junction, TEdgeVector & edges) const override;

  /// Removes all fake turns and vertices from the graph.
  void ResetFakes();

  /// Adds fake edges from fake position rp to real vicinity
  /// positions.
  void AddFakeEdges(Junction const & junction, vector<pair<Edge, Junction>> const & vicinities);

  /// Returns RoadInfo for a road corresponding to featureId.
  virtual RoadInfo GetRoadInfo(FeatureID const & featureId) const = 0;

  /// Returns speed in KM/H for a road corresponding to featureId.
  virtual double GetSpeedKMPH(FeatureID const & featureId) const = 0;

  /// Returns speed in KM/H for a road corresponding to edge.
  double GetSpeedKMPH(Edge const & edge) const;

  /// Calls edgesLoader on each feature which is close to cross.
  virtual void ForEachFeatureClosestToCross(m2::PointD const & cross,
                                            ICrossEdgesLoader & edgesLoader) const = 0;

  /// Finds the closest edges to the point.
  /// @return Array of pairs of Edge and projection point on the Edge. If there is no the closest edges
  /// then returns empty array.
  virtual void FindClosestEdges(m2::PointD const & point, uint32_t count,
                                vector<pair<Edge, Junction>> & vicinities) const = 0;

  /// @return Types for the specified feature
  virtual void GetFeatureTypes(FeatureID const & featureId, feature::TypesHolder & types) const = 0;

  /// @return Types for the specified edge
  virtual void GetEdgeTypes(Edge const & edge, feature::TypesHolder & types) const override;

  virtual IRoadGraph::Mode GetMode() const = 0;

  /// Clear all temporary buffers.
  virtual void ClearState() {}

  /// \brief Finds all outgoing regular (non-fake) edges for junction.
  void GetRegularOutgoingEdges(Junction const & junction, TEdgeVector & edges) const;
  /// \brief Finds all ingoing regular (non-fake) edges for junction.
  void GetRegularIngoingEdges(Junction const & junction, TEdgeVector & edges) const;
  /// \brief Finds all outgoing fake edges for junction.
  void GetFakeOutgoingEdges(Junction const & junction, TEdgeVector & edges) const;
  /// \brief Finds all ingoing fake edges for junction.
  void GetFakeIngoingEdges(Junction const & junction, TEdgeVector & edges) const;

private:
  template <typename Fn>
  void ForEachFakeEdge(Fn && fn)
  {
    for (auto const & m : m_fakeIngoingEdges)
    {
      for (auto const & e : m.second)
        fn(e);
    }

    for (auto const & m : m_fakeOutgoingEdges)
    {
      for (auto const & e : m.second)
        fn(e);
    }
  }

  map<Junction, TEdgeVector> m_fakeIngoingEdges;
  map<Junction, TEdgeVector> m_fakeOutgoingEdges;
};

string DebugPrint(IRoadGraph::Mode mode);

IRoadGraph::RoadInfo MakeRoadInfoForTesting(bool bidirectional, double speedKMPH,
                                            initializer_list<m2::PointD> const & points);

inline void JunctionsToPoints(vector<Junction> const & junctions, vector<m2::PointD> & points)
{
  points.resize(junctions.size());
  for (size_t i = 0; i < junctions.size(); ++i)
    points[i] = junctions[i].GetPoint();
}

inline void JunctionsToAltitudes(vector<Junction> const & junctions, feature::TAltitudes & altitudes)
{
  altitudes.resize(junctions.size());
  for (size_t i = 0; i < junctions.size(); ++i)
    altitudes[i] = junctions[i].GetAltitude();
}
}  // namespace routing
