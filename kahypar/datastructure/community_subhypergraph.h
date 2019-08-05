/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2015-2016 Sebastian Schlag <sebastian.schlag@kit.edu>
 *
 * KaHyPar is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KaHyPar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KaHyPar.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#pragma once

#include <vector>

#include "kahypar/datastructure/hypergraph.h"
#include "kahypar/utils/thread_pool.h"

namespace kahypar {
namespace ds {

template <typename Hypergraph>
struct CommunityHyperedge {
  using HyperedgeID = typename Hypergraph::HyperedgeID;

  CommunityHyperedge(const HyperedgeID original_he,
                              const size_t incidence_array_start,
                              const size_t incidence_array_end) :
    original_he(original_he),
    incidence_array_start(incidence_array_start),
    incidence_array_end(incidence_array_end) { }

  HyperedgeID original_he;
  size_t incidence_array_start;
  size_t incidence_array_end;
};

template <typename Hypergraph>
bool operator==(const CommunityHyperedge<Hypergraph>& lhs, const CommunityHyperedge<Hypergraph>& rhs) {
  return lhs.original_he == rhs.original_he &&
         lhs.incidence_array_start == rhs.incidence_array_start &&
         lhs.incidence_array_end == rhs.incidence_array_end;
}

template <typename Hypergraph>
struct CommunitySubhypergraph {
  using HypernodeID = typename Hypergraph::HypernodeID;
  using HyperedgeID = typename Hypergraph::HyperedgeID;
  using PartitionID = typename Hypergraph::PartitionID;

  explicit CommunitySubhypergraph(const Hypergraph& hg, const PartitionID community_id) :
    hg(hg),
    community_id(community_id),
    num_hn_not_in_community(0),
    num_pins_not_in_community(0),
    subhypergraph(new Hypergraph()),
    subhypergraph_to_hypergraph_hn(),
    subhypergraph_to_hypergraph_he() { }

  CommunitySubhypergraph(CommunitySubhypergraph&& other) = default;

  void addHypernode(const HypernodeID original_hn) {
    subhypergraph_to_hypergraph_hn.push_back(original_hn);
    num_hn_not_in_community += (hg.communityID(original_hn) != community_id) ? 1 : 0;
  }

  void addPin(const HypernodeID original_pin) {
    num_pins_not_in_community += (hg.communityID(original_pin) != community_id) ? 1 : 0;
  }

  void sortHypernodes() {
    std::sort(subhypergraph_to_hypergraph_hn.begin(), subhypergraph_to_hypergraph_hn.end());
  }

  void addHyperedge(const HyperedgeID original_he, 
                    const size_t incidence_array_start, 
                    const size_t incidence_array_end) {
    subhypergraph_to_hypergraph_he.emplace_back(original_he, incidence_array_start, incidence_array_end);
  }

  const Hypergraph& hg;
  PartitionID community_id;
  size_t num_hn_not_in_community;
  size_t num_pins_not_in_community;
  std::unique_ptr<Hypergraph> subhypergraph;
  std::vector<HypernodeID> subhypergraph_to_hypergraph_hn;
  std::vector<CommunityHyperedge<Hypergraph>> subhypergraph_to_hypergraph_he;
};

/**
 * Extracts the community-induced section subhypergraph from the original hypergraph.
 * We define this subhypergraph as H x ( V(C) u V' ) where C is the corresponding community,
 * V(C) = { v | v \in C } and V' = { v | \exists e \in E: v \in e \ V(C) }. V' corresponds to
 * all hypernodes, which are not in C, but are connected to the community via an hyperedge e. For
 * the definition of the notation H x V, we refer to the wikipedia article for hypergraphs.
 * 
 * This function will be used during parallel coarsening, where we extract a community from
 * the original hypergraph an coarsen inside it independently. However, to ensure that
 * the ratings of the coarsener are matching those of the sequential partitioner, we need
 * the original hyperedge sizes of the subhypergraph induced by community C, which includes
 * also the hyperedges only partially contained in that subhypergraph.
 */
template <typename Hypergraph>
CommunitySubhypergraph<Hypergraph>
extractCommunityInducedSectionHypergraph(const Hypergraph& hypergraph,
                                         const typename Hypergraph::PartitionID community,
                                         bool respect_order_of_hypernodes) {
  using HypernodeID = typename Hypergraph::HypernodeID;
  using HyperedgeID = typename Hypergraph::HyperedgeID;
  using PartitionID = typename Hypergraph::PartitionID;
  using CommunitySubhypergraph = CommunitySubhypergraph<Hypergraph>;

  std::unordered_map<HypernodeID, HypernodeID> hypergraph_to_subhypergraph;
  CommunitySubhypergraph community_subhypergraph(hypergraph, community);

  HypernodeID num_hypernodes = 0;
  std::vector<bool> visited(hypergraph.initialNumNodes() + hypergraph.initialNumEdges(), false);
  for ( const HypernodeID& hn : hypergraph.nodes() ) {
    if ( hypergraph.communityID(hn) == community ) {
      // Add all neighbors of hypernode to subhypergraph in order to ensure
      // that each incident hyperedge of hn is fully contained in the subhypergraph
      for ( const HyperedgeID& he : hypergraph.incidentEdges(hn) ) {
        if ( !visited[hypergraph.initialNumNodes() + he] ) {
          for ( const HypernodeID& pin : hypergraph.pins(he) ) {
            if ( !visited[pin] ) {
              community_subhypergraph.addHypernode(pin);
              visited[pin] = true;
            }
            community_subhypergraph.addPin(pin);
          }
          visited[hypergraph.initialNumNodes() + he] = true;
        }
      }
    }
  }

  // Makes it easier to test, if numbering of hypernodes is in the same order than
  // in the original hypergraph
  if ( respect_order_of_hypernodes ) {
    community_subhypergraph.sortHypernodes();
  }

  // Create hypergraph to subhypergraph mapping
  for ( const HypernodeID& hn : community_subhypergraph.subhypergraph_to_hypergraph_hn ) {
    hypergraph_to_subhypergraph[hn] = num_hypernodes++;
  }

  if ( num_hypernodes > 0 ) {
    community_subhypergraph.subhypergraph->_hypernodes.resize(num_hypernodes);
    community_subhypergraph.subhypergraph->_num_hypernodes = num_hypernodes;  

    HyperedgeID num_hyperedges = 0;
    HypernodeID pin_index = 0;
    SparseMap<PartitionID, size_t> community_sizes_in_he(hypergraph.initialNumNodes());
    for ( const HyperedgeID& he : hypergraph.edges() ) {
      // Add all hyperedges with all its pins to the subhypergraph which we visited before
      if ( visited[hypergraph.initialNumNodes() + he] ) {
        community_subhypergraph.subhypergraph->_hyperedges.emplace_back(0, 0, hypergraph.edgeWeight(he));
        ++community_subhypergraph.subhypergraph->_num_hyperedges;
        community_subhypergraph.subhypergraph->_hyperedges[num_hyperedges].setFirstEntry(pin_index);
        community_sizes_in_he.clear();
        for (const HypernodeID& pin : hypergraph.pins(he)) {
          ASSERT(hypergraph_to_subhypergraph.find(pin) != hypergraph_to_subhypergraph.end(), 
                 "Subhypergraph does not contain hypernode " << pin);
          community_subhypergraph.subhypergraph->hyperedge(num_hyperedges).incrementSize();
          community_subhypergraph.subhypergraph->hyperedge(num_hyperedges).hash += math::hash(hypergraph_to_subhypergraph[pin]);
          community_subhypergraph.subhypergraph->_incidence_array.push_back(hypergraph_to_subhypergraph[pin]);
          community_sizes_in_he[hypergraph.communityID(pin)] += hypergraph.nodeWeight(pin);
          ++pin_index;
        }
        ++num_hyperedges;

        // Define unique range in incidence array such that the pins of hyperedge he
        // which belongs to the current community can be written back to the original
        // hypergraph incidence array without conflicts (when writting in parallel)
        size_t incidence_array_start = 0;
        size_t community_size = 0;
        for ( const auto& element : community_sizes_in_he ) {
          const PartitionID& comm = element.key;
          if ( comm < community ) {
            incidence_array_start += element.value;
          } else if ( comm == community ) {
            community_size = element.value;
          }
        }
        community_subhypergraph.addHyperedge(he, incidence_array_start, incidence_array_start + community_size);
      }
    }
    
    setupInternalStructure(hypergraph, community_subhypergraph.subhypergraph_to_hypergraph_hn, 
                           *community_subhypergraph.subhypergraph,
                           2, num_hypernodes, pin_index, num_hyperedges);
  }

  return community_subhypergraph;
}

template <typename Hypergraph>
void mergeCommunityInducedSectionHypergraphs(kahypar::parallel::ThreadPool& pool,
                                             Hypergraph& hypergraph,
                                             const std::vector<CommunitySubhypergraph<Hypergraph>>& communities,
                                             const std::vector<typename Hypergraph::ContractionMemento>& history) {
  using HypernodeID = typename Hypergraph::HypernodeID;
  using HyperedgeID = typename Hypergraph::HyperedgeID;
  using PartitionID = typename Hypergraph::PartitionID;
  using Hypernode = typename Hypergraph::Hypernode;
  using Hyperedge = typename Hypergraph::Hyperedge;
  using CommunityHyperedge = CommunityHyperedge<Hypergraph>;
  using CommunitySubhypergraph = CommunitySubhypergraph<Hypergraph>;

  // PRE-PHASE
  hypergraph._current_num_hypernodes = 0;
  hypergraph._current_num_pins = 0;
  for ( const CommunitySubhypergraph& community : communities ) {
    hypergraph._current_num_hypernodes += (community.subhypergraph->currentNumNodes() - community.num_hn_not_in_community);
    hypergraph._current_num_pins += (community.subhypergraph->currentNumPins() - community.num_pins_not_in_community);
    hypergraph._current_num_hyperedges -= (community.subhypergraph->initialNumEdges() - community.subhypergraph->currentNumEdges());
  }

  // PHASE 1
  // In the first phase we write for each community subhypergraph the 
  // hypernodes that belong the corresponding community back to the 
  // original hypergraph and to the incidence array. For writing back to
  // the incidence array we are using the unique ranges for the pins of the
  // community defined in the CommunityHyperedge.
  for ( const CommunitySubhypergraph& community : communities ) {
    pool.enqueue([&hypergraph, &community]() {
      const PartitionID current_community = community.community_id;
      const Hypergraph& community_subhypergraph = *community.subhypergraph;

      std::vector<bool> visited(community_subhypergraph.initialNumEdges(), false);
      for ( HypernodeID hn = 0; hn < community_subhypergraph.initialNumNodes(); ++hn ) {
        if ( community_subhypergraph._communities[hn] == current_community ) {
          const HypernodeID original_hn = community.subhypergraph_to_hypergraph_hn[hn];
          ASSERT(original_hn < hypergraph._hypernodes.size(), "Hypernode " << original_hn << " does not exists in original hypergraph");
          ASSERT(current_community == hypergraph._communities[original_hn], "Hypernode " << original_hn << " differs from its community in "
            << "the community subhypergraph");
          std::vector<HyperedgeID> incident_nets;
          for ( const HyperedgeID& he : community_subhypergraph._hypernodes[hn].incidentNets() ) {
            // Map incident hyperedge back to the original hypergraph
            // Note, a hypernode can only get new incident net when contracting it with another hypernode.
            // Since, we only contract inside a community subhypergraph the incident nets for a given
            // hypernode that belongs to the current community are given by the incident net structure of
            // the current community subhypergraph (its not possible that due to an contraction within an other
            // community a hypernode in a different community gets additional incident nets)
            const HyperedgeID original_he = community.subhypergraph_to_hypergraph_he[he].original_he;
            ASSERT(original_he < hypergraph._hyperedges.size(), "Hyperedge " << original_he << " does not exists in original hypergraph");
            incident_nets.push_back(original_he);

            if ( !visited[he] ) {
              const CommunityHyperedge& community_hyperedge = community.subhypergraph_to_hypergraph_he[he];
              size_t original_incidence_array_start = hypergraph._hyperedges[original_he].firstEntry() + 
                                                        community_hyperedge.incidence_array_start;
              size_t incidence_array_start = community_subhypergraph._hyperedges[he].firstEntry();
              size_t incidence_array_end = community_subhypergraph._hyperedges[he + 1].firstEntry();
              for ( size_t i = incidence_array_start; i < incidence_array_end; ++i ) {
                const HypernodeID pin = community_subhypergraph._incidence_array[i];
                if ( community_subhypergraph._communities[pin] == current_community ) {
                  // If the pin belongs to current community we write it back to incidence array
                  // of the original hypergraph in the range given by the corresponding CommunityHyperedge
                  const HypernodeID original_pin = community.subhypergraph_to_hypergraph_hn[pin];
                  ASSERT(original_pin < hypergraph._hypernodes.size(), "Hypernode " << original_pin << " does not exists in original hypergraph");
                  ASSERT(current_community == hypergraph._communities[original_pin], "Hypernode " << original_pin << " differs from its community in "
                    << "the community subhypergraph");
                  hypergraph._incidence_array[original_incidence_array_start++] = original_pin;
                }
              }
              // Update weight
              if ( hypergraph._hyperedges[he].weight() < community_subhypergraph._hyperedges[he].weight() ) {
                hypergraph._hyperedges[original_he].setWeight(community_subhypergraph._hyperedges[he].weight());
              }
              // Disable hyperedge
              // Note, a hyperedge is disabled during coarsening, if it becomes parallel with an other hyperedge or
              // a single-pin net. Both cases can only happen at most one time among all community subhypergraphs for
              // a hyperedge. Since, we only hypernodes within the same community. Consequently, for all community subhypergraphs
              // this statement can evaluate only one time to true.
              if ( community_subhypergraph._hyperedges[he].isDisabled() ) {
                hypergraph._hyperedges[original_he].disable();
              }
              ASSERT(original_incidence_array_start == hypergraph._hyperedges[original_he].firstEntry() + community_hyperedge.incidence_array_end,
                      "Number of pins of hyperedge " << original_he << " differs from the number of pins in subhypergraph for community " << current_community
                      << "(" << V(original_incidence_array_start) << ", " 
                      << V(hypergraph._hyperedges[original_he].firstEntry() + community_hyperedge.incidence_array_end) << ")" );
              visited[he] = true;
            } 
          }
          hypergraph._hypernodes[original_hn] = Hypernode(incident_nets, community_subhypergraph._hypernodes[hn].weight(),
                                                          !community_subhypergraph._hypernodes[hn].isDisabled());
        }
      }
    });
  }

  // Barrier
  pool.loop_until_empty();

  // PHASE 2
  // All disabled hypernodes have to follow a specific order in invalid part of the incidence array
  // such that they can be successfully uncontracted. They have be sorted in decreasing order of their
  // contraction. In order to realize this we compute the contraction index of a hypernode inside the
  // contraction history and use it later for sorting them.
  std::vector<int> contraction_index(hypergraph.initialNumNodes(), -1);
  std::function<void(const size_t, const size_t)> construct_contraction_index = 
    [&history, &contraction_index](const size_t start, const size_t end) {
    for ( size_t i = start; i < end; ++i ) {
      const HypernodeID hn = history[i].v;
      ASSERT(contraction_index[hn] == -1, "Hypernode " << hn << " occurs more than one time in the contraction history");
      contraction_index[hn] = i;
    }
  };

  size_t num_threads = pool.size();
  size_t step = history.size() / num_threads;
  if ( step >= 1 && num_threads != 1) {
    for ( size_t i = 0; i < num_threads; ++i ) {
      size_t start = i * step;
      size_t end = ( i == num_threads - 1 ) ? history.size() : ( ( i + 1 ) * step );
      pool.enqueue(construct_contraction_index, start, end);
    }
  } else {
    construct_contraction_index(0, history.size());
  }

  // Barrier
  pool.loop_until_empty();

  // PHASE 3
  // The incidence array of a hyperedge is constructed as follows: The first part consists
  // of all enabled pins and the remainder of all invalid pins. The invalid pins in the
  // remainder are sorted in decreasing order of their contraction index.
  std::function<void(const size_t, const size_t)> create_contraction_hierarchy = 
    [&hypergraph, &contraction_index](const HyperedgeID start, const HyperedgeID end) {
    for ( HyperedgeID he = start; he < end; ++he ) {
      Hyperedge& current_he = hypergraph._hyperedges[he];
      bool isDisabled = current_he.isDisabled();
      if ( isDisabled ) {
        current_he.enable();
      }
      hypergraph.edgeHash(he) = Hypergraph::kEdgeHashSeed;
      for ( size_t j = current_he.firstEntry(); j < current_he.firstInvalidEntry(); ++j ) {
        const HypernodeID pin = hypergraph._incidence_array[j];
        if ( hypergraph._hypernodes[pin].isDisabled() ) {
          // Swap disabled pins in remainder and decrement size of hyperedge
          std::swap(hypergraph._incidence_array[j], hypergraph._incidence_array[current_he.firstInvalidEntry() - 1]);
          current_he.decrementSize();
          --j;
        } else {
          // Otherwise update hash
          hypergraph.edgeHash(he) += math::hash(pin);
        }
      }
      if ( isDisabled ) {
        current_he.disable();
      }
      // Sort remainder in decreasing order of their contraction index.
      size_t invalid_pins_start = current_he.firstInvalidEntry();
      size_t invalid_pins_end = hypergraph._hyperedges[he + 1].firstEntry();
      std::sort(hypergraph._incidence_array.begin() + invalid_pins_start,
                hypergraph._incidence_array.begin() + invalid_pins_end,
                [&contraction_index](const HypernodeID& u, const HypernodeID& v) {
                  return contraction_index[u] > contraction_index[v];
                });
    }
  };

  step = hypergraph.initialNumEdges() / num_threads;
  if ( step >= 1 && num_threads != 1) {
    for ( size_t i = 0; i < num_threads; ++i ) {
      HyperedgeID start = i * step;
      HyperedgeID end = ( i == num_threads - 1 ) ? hypergraph.initialNumEdges() : ( ( i + 1 ) * step );
      pool.enqueue(create_contraction_hierarchy, start, end);
    }
  } else {
    create_contraction_hierarchy(0, hypergraph.initialNumEdges());
  }

  // Barrier
  pool.loop_until_empty();
}


}  // namespace ds
}  // namespace kahypar
