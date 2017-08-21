/** partitioned graph wrapper -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 * @author Roshan Dathathri <roshan@cs.utexas.edu>
 * @author Loc Hoang <l_hoang@utexas.edu> (Thread Range stuff)
 */

#include "Galois/gstl.h"
#include "Galois/Graphs/LC_CSR_Graph.h"
#include "Galois/Runtime/Substrate.h"
#include "Galois/Runtime/Network.h"

#include "Galois/Runtime/Serialize.h"
#include "Galois/Statistic.h"

#include "Galois/Runtime/GlobalObj.h"
#include "Galois/Runtime/OfflineGraph.h"
#include <vector>
#include <set>
#include <algorithm>
#include <unordered_map>
#include <iostream>

#include "Galois/Runtime/DataCommMode.h"
#include "Galois/Runtime/Dynamic_bitset.h"
#include <fcntl.h>
#include <sys/mman.h>

// for thread container stuff
#include "Galois/Substrate/ThreadPool.h"

//#include "Galois/Runtime/dGraph_vertexCut.h"
//#include "Galois/Runtime/dGraph_edgeCut.h"

#ifdef __GALOIS_HET_CUDA__
#include "Galois/Runtime/Cuda/cuda_mtypes.h"
#endif

#ifdef __GALOIS_HET_OPENCL__
#include "Galois/OpenCL/CL_Header.h"
#endif

#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_BARE_MPI_COMMUNICATION__
#include "mpi.h"
#endif
#endif

#ifndef _GALOIS_DIST_HGRAPH_H
#define _GALOIS_DIST_HGRAPH_H

namespace cll = llvm::cl;
#ifdef __GALOIS_EXP_COMMUNICATION_ALGORITHM__
static cll::opt<unsigned> buffSize("sendBuffSize", 
                       cll::desc("max size for send buffers in element count"), 
                       cll::init(4096));
#endif

cll::opt<bool> useGidMetadata("useGidMetadata", 
  cll::desc("Use Global IDs in indices metadata (only when -metadata=2)"), 
  cll::init(false));

static cll::opt<bool> useNumaAlloc("useNumaAlloc", 
                             cll::desc("Setting to use numa allocation"),
                             cll::init(false));

static cll::opt<bool> edgeNuma("edgeNuma", 
                             cll::desc("Flag to use exp. edge-centric "
                             "numa allocation for threads"),
                             cll::init(false));

enum MASTERS_DISTRIBUTION {
  BALANCED_MASTERS, BALANCED_EDGES_OF_MASTERS, BALANCED_MASTERS_AND_EDGES
};

static cll::opt<MASTERS_DISTRIBUTION> masters_distribution("balanceMasters",
                                     cll::desc("Type of masters distribution."),
                                     cll::values(
                                       clEnumValN(BALANCED_MASTERS, "nodes", 
                                                  "Balance nodes only"), 
                                       clEnumValN(BALANCED_EDGES_OF_MASTERS, "edges", 
                                                  "Balance edges only (default)"), 
                                       clEnumValN(BALANCED_MASTERS_AND_EDGES, "both", 
                                                  "Balance both nodes and edges"), 
                                       clEnumValEnd),
                                     cll::init(BALANCED_EDGES_OF_MASTERS));

static cll::opt<uint32_t> nodeWeightOfMaster("nodeWeight", 
                             cll::desc("Determines weight of nodes when "
                             "distributing masterst to hosts"),
                             cll::init(0));

static cll::opt<uint32_t> edgeWeightOfMaster("edgeWeight", 
                             cll::desc("Determines weight of edges when "
                             "distributing masters to hosts"),
                             cll::init(0));

static cll::opt<uint32_t> nodeAlphaRanges("nodeAlphaRanges", 
                             cll::desc("Determines weight of nodes when "
                             "partitioning among threads"),
                             cll::init(0));

enum WriteLocation { writeSource, writeDestination, writeAny };
enum ReadLocation { readSource, readDestination, readAny };

template<typename NodeTy, typename EdgeTy, bool BSPNode = false, bool BSPEdge = false>
class hGraph: public GlobalObject {

 public:
  typedef typename std::conditional<
    BSPNode, std::pair<NodeTy, NodeTy>, NodeTy>::type realNodeTy;
  typedef typename std::conditional<
    BSPEdge && !std::is_void<EdgeTy>::value, std::pair<EdgeTy, EdgeTy>, EdgeTy>::type realEdgeTy;

  typedef typename Galois::Graph::LC_CSR_Graph<realNodeTy, realEdgeTy, true> GraphTy;

  enum SyncType { syncReduce, syncBroadcast };

  GraphTy graph;
  bool transposed;
  bool round;
  uint64_t totalNodes; // Total nodes in the complete graph.
  uint64_t totalEdges;
  uint64_t totalMirrorNodes; // Total mirror nodes from others.
  uint64_t totalOwnedNodes; // Total owned nodes in accordance with graphlab.
  uint32_t numOwned; // [0, numOwned) = global nodes owned, thus [numOwned, numNodes are replicas
  uint64_t numOwned_edges; // [0, numOwned) = global nodes owned, thus [numOwned, numNodes are replicas
  uint32_t total_isolatedNodes; // Calculate the total isolated nodes
  uint64_t globalOffset; // [numOwned, end) + globalOffset = GID
  const unsigned id; // my hostid // FIXME: isn't this just Network::ID?
  const uint32_t numHosts;
  //ghost cell ID translation

  uint32_t numNodes; // Total nodes created on a host (Master + slaves)
  uint32_t numNodesWithEdges; // Total nodes that need to be executed in an operator.
  uint32_t beginMaster; // local id of the beginning of master nodes.
  uint32_t endMaster; // local id of the end of master nodes.

  // Master nodes on each host.
  std::vector<std::pair<uint64_t, uint64_t>> gid2host;
  uint64_t last_nodeID_withEdges_bipartite;

  std::vector<uint32_t> masterRanges;
  std::vector<uint32_t> withEdgeRanges;

  std::vector<Galois::Runtime::SpecificRange<boost::counting_iterator<size_t>>>
    specificRanges;

  //memoization optimization
  std::vector<std::vector<size_t>> mirrorNodes; // mirror nodes from different hosts. For reduce
  std::vector<std::vector<size_t>> masterNodes; // master nodes on different hosts. For broadcast

  /****** VIRTUAL FUNCTIONS *********/
  virtual uint32_t G2L(uint64_t) const = 0 ;
  virtual uint64_t L2G(uint32_t) const = 0;
  virtual bool is_vertex_cut() const = 0;
  virtual unsigned getHostID(uint64_t) const = 0;
  virtual size_t getOwner_lid(size_t lid) const {
    auto gid = L2G(lid);
    return getHostID(gid);
  }
  virtual bool isOwned(uint64_t) const = 0;
  virtual bool isLocal(uint64_t) const = 0;
  virtual uint64_t get_local_total_nodes() const = 0;

#if 0
  virtual void save_meta_file(std::string name) const {
    std::cout << "Base implementation doesn't do anything\n";
  }
#endif

  // requirement: for all X and Y,
  // On X, nothingToSend(Y) <=> On Y, nothingToRecv(X)
  // Note: templates may not be virtual, so passing types as arguments
  virtual bool nothingToSend(unsigned host, SyncType syncType, WriteLocation writeLocation, ReadLocation readLocation) { // ignore write/read location 
     auto &sharedNodes = (syncType == syncReduce) ? mirrorNodes : masterNodes;
     return (sharedNodes[host].size() == 0);
  }
  virtual bool nothingToRecv(unsigned host, SyncType syncType, WriteLocation writeLocation, ReadLocation readLocation) { // ignore write/read location 
     auto &sharedNodes = (syncType == syncReduce) ? masterNodes : mirrorNodes;
     return (sharedNodes[host].size() == 0);
  }

  virtual void reset_bitset(SyncType syncType, void (*bitset_reset_range)(size_t, size_t)) const = 0;

#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
  unsigned comm_mode; // Communication mode: 0 - original, 1 - simulated net, 2 - simulated bare MPI
#endif
#endif


   uint32_t numPipelinedPhases;
   uint32_t num_recv_expected; // Number of receives expected for local completion.
   uint32_t num_run; //Keep track of number of runs.
   uint32_t num_iteration; //Keep track of number of iterations.

  //Stats: for rough estimate of sendBytes.
   Galois::Statistic statGhostNodes;

   /****** checkpointing **********/
   Galois::Runtime::RecvBuffer checkpoint_recvBuffer;
  // Select from edgeCut or vertexCut
  //typedef typename std::conditional<PartitionTy, DS_vertexCut ,DS_edgeCut>::type DS_type;
  //DS_type DS;


   template<bool en, typename std::enable_if<en>::type* = nullptr>
   NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
      auto& r = graph.getData(N, mflag);
      return round ? r.first : r.second;
   }

   template<bool en, typename std::enable_if<!en>::type* = nullptr>
   NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
      auto& r = graph.getData(N, mflag);
      return r;
   }

   template<bool en, typename std::enable_if<en>::type* = nullptr>
   const NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) const {
      auto& r = graph.getData(N, mflag);
      return round ? r.first : r.second;
   }

   template<bool en, typename std::enable_if<!en>::type* = nullptr>
   const NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) const {
      auto& r = graph.getData(N, mflag);
      return r;
   }
   template<bool en, typename std::enable_if<en>::type* = nullptr>
   typename GraphTy::edge_data_reference getEdgeDataImpl(typename GraphTy::edge_iterator ni, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
      auto& r = graph.getEdgeData(ni, mflag);
      return round ? r.first : r.second;
   }

   template<bool en, typename std::enable_if<!en>::type* = nullptr>
   typename GraphTy::edge_data_reference getEdgeDataImpl(typename GraphTy::edge_iterator ni, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
      auto& r = graph.getEdgeData(ni, mflag);
      return r;
   }

private:

  // compute owners by blocking Nodes
  void computeMastersBlockedNodes(Galois::Graph::OfflineGraph& g, 
      uint64_t numNodes_to_divide, std::vector<unsigned>& scalefactor) {
    if (scalefactor.empty() || (numHosts == 1)) {
      for (unsigned i = 0; i < numHosts; ++i)
        gid2host.push_back(Galois::block_range(
                             0U, (unsigned)numNodes_to_divide, i, 
                             numHosts));
    } else {
      assert(scalefactor.size() == numHosts);

      unsigned numBlocks = 0;
      for (unsigned i = 0; i < numHosts; ++i)
        numBlocks += scalefactor[i];

      std::vector<std::pair<uint64_t, uint64_t>> blocks;
      for (unsigned i = 0; i < numBlocks; ++i)
        blocks.push_back(Galois::block_range(
                           0U, (unsigned)numNodes_to_divide, i, numBlocks));

      std::vector<unsigned> prefixSums;
      prefixSums.push_back(0);
      for (unsigned i = 1; i < numHosts; ++i)
        prefixSums.push_back(prefixSums[i - 1] + scalefactor[i - 1]);
      for (unsigned i = 0; i < numHosts; ++i) {
        unsigned firstBlock = prefixSums[i];
        unsigned lastBlock = prefixSums[i] + scalefactor[i] - 1;
        gid2host.push_back(std::make_pair(blocks[firstBlock].first, 
                                          blocks[lastBlock].second));
      }
    }
  }

  // compute owners while trying to balance edges
  void computeMastersBalancedEdges(Galois::Graph::OfflineGraph& g,
      uint64_t numNodes_to_divide, std::vector<unsigned>& scalefactor) {
    if (edgeWeightOfMaster == 0) {
      edgeWeightOfMaster = 1;
    }
    auto& net = Galois::Runtime::getSystemNetworkInterface();
#ifdef SERIALIZE_COMPUTE_MASTERS
    if (id == 0) {
      // compute owners for all hosts and send that info to all hosts
      Galois::prefix_range(g, (uint64_t)0U, numNodes_to_divide,
                           numHosts,
                           gid2host, 0, scalefactor);
      for (unsigned h = 1; h < numHosts; ++h) {
        Galois::Runtime::SendBuffer b;
        Galois::Runtime::gSerialize(b, gid2host);
        net.sendTagged(h, Galois::Runtime::evilPhase, b);
      }
      net.flush();
    } else {
      // receive computed owners from host 0
      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      do {
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      } while (!p);
      assert(p->first == 0);
      auto& b = p->second;
      Galois::Runtime::gDeserialize(b, gid2host);
    }
    ++Galois::Runtime::evilPhase;
#else
    gid2host.resize(numHosts);
    auto r = g.divideByNode(0, edgeWeightOfMaster, id, numHosts, scalefactor);
    gid2host[id].first = *(r.first.first);
    gid2host[id].second = *(r.first.second);

    //printf("[%d] first is %lu, second is %lu\n", id, gid2host[id].first, gid2host[id].second);
    //printf("[%d] first edge is %lu, second edge is %lu\n", id, *(r.second.first), *(r.second.second));
    //std::cout << "id " << id << " " << gid2host[id].first << " " << gid2host[id].second << "\n";

    for (unsigned h = 0; h < numHosts; ++h) {
      if (h == id) continue;
      Galois::Runtime::SendBuffer b;
      Galois::Runtime::gSerialize(b, gid2host[id]);
      net.sendTagged(h, Galois::Runtime::evilPhase, b);
    }
    net.flush();
    unsigned received = 1;
    while (received < numHosts) {
      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      do {
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      } while (!p);
      assert(p->first != id);
      auto& b = p->second;
      Galois::Runtime::gDeserialize(b, gid2host[p->first]);
      ++received;
    }
    ++Galois::Runtime::evilPhase;
#endif
  }

  // compute owners while trying to balance nodes and edges
  void computeMastersBalancedNodesAndEdges(Galois::Graph::OfflineGraph& g,
      uint64_t numNodes_to_divide, std::vector<unsigned>& scalefactor) {
    if (nodeWeightOfMaster == 0) {
      nodeWeightOfMaster = g.sizeEdges() / g.size(); // average degree
    }
    if (edgeWeightOfMaster == 0) {
      edgeWeightOfMaster = 1;
    }
    auto& net = Galois::Runtime::getSystemNetworkInterface();
#ifdef SERIALIZE_COMPUTE_MASTERS
    if (id == 0) {
      // compute owners for all hosts and send that info to all hosts
      Galois::prefix_range(g, (uint64_t)0U, numNodes_to_divide,
                           numHosts,
                           gid2host, nodeWeightOfMaster, scalefactor);
      for (unsigned h = 1; h < numHosts; ++h) {
        Galois::Runtime::SendBuffer b;
        Galois::Runtime::gSerialize(b, gid2host);
        net.sendTagged(h, Galois::Runtime::evilPhase, b);
      }
      net.flush();
    } else {
      // receive computed owners from host 0
      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      do {
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      } while (!p);
      assert(p->first == 0);
      auto& b = p->second;
      Galois::Runtime::gDeserialize(b, gid2host);
    }
    ++Galois::Runtime::evilPhase;
#else
    gid2host.resize(numHosts);
    auto r = g.divideByNode(nodeWeightOfMaster, edgeWeightOfMaster, id, numHosts, scalefactor);
    gid2host[id].first = *r.first.first;
    gid2host[id].second = *r.first.second;
    for (unsigned h = 0; h < numHosts; ++h) {
      if (h == id) continue;
      Galois::Runtime::SendBuffer b;
      Galois::Runtime::gSerialize(b, gid2host[id]);
      net.sendTagged(h, Galois::Runtime::evilPhase, b);
    }
    net.flush();
    unsigned received = 1;
    while (received < numHosts) {
      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      do {
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      } while (!p);
      assert(p->first != id);
      auto& b = p->second;
      Galois::Runtime::gDeserialize(b, gid2host[p->first]);
      ++received;
    }
    ++Galois::Runtime::evilPhase;
#endif
  }

protected:

  uint64_t computeMasters(Galois::Graph::OfflineGraph& g,
      std::vector<unsigned>& scalefactor,
      bool isBipartite = false) {
    Galois::Timer timer;
    timer.start();
    g.reset_seek_counters();

    uint64_t numNodes_to_divide = 0;

    if (isBipartite) {
      for (uint64_t n = 0; n < g.size(); ++n){
        if(std::distance(g.edge_begin(n), g.edge_end(n))){
                ++numNodes_to_divide;
                last_nodeID_withEdges_bipartite = n;
        }
      }
    } else {
      numNodes_to_divide = g.size();
    }

    // compute masters for all nodes
    switch(masters_distribution) {
      case BALANCED_MASTERS:
        computeMastersBlockedNodes(g, numNodes_to_divide, scalefactor);
        break;
      case BALANCED_MASTERS_AND_EDGES:
        computeMastersBalancedNodesAndEdges(g, numNodes_to_divide, scalefactor);
        break;
      case BALANCED_EDGES_OF_MASTERS:
      default:
        computeMastersBalancedEdges(g, numNodes_to_divide, scalefactor);
        break;
    }

    timer.stop();
    fprintf(stderr, "[%u] Master distribution time : %f seconds to read %lu bytes in %lu seeks (%f MBPS)\n", 
        id, timer.get_usec()/1000000.0f, g.num_bytes_read(), g.num_seeks(), g.num_bytes_read()/(float)timer.get_usec());
    return numNodes_to_divide;
  }

public:
   GraphTy & getGraph() {
      return graph;
   }
#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
  void set_comm_mode(unsigned mode) { // Communication mode: 0 - original, 1 - simulated net, 2 - simulated bare MPI
    comm_mode = mode;
  }
#endif
#endif
  static void syncRecv(uint32_t src, Galois::Runtime::RecvBuffer& buf) {
      uint32_t oid;
      void (hGraph::*fn)(Galois::Runtime::RecvBuffer&);
      Galois::Runtime::gDeserialize(buf, oid, fn);
      hGraph* obj = reinterpret_cast<hGraph*>(ptrForObj(oid));
      (obj->*fn)(buf);
   }

   void exchange_info_landingPad(Galois::Runtime::RecvBuffer& buf){
     uint32_t hostID;
     uint64_t numItems;
     std::vector<uint64_t> items;
     Galois::Runtime::gDeserialize(buf, hostID, numItems);

     Galois::Runtime::gDeserialize(buf, masterNodes[hostID]);
     //std::cout << "from : " << hostID << " -> " << numItems << " --> " << masterNodes[hostID].size() << "\n";
   }

   template<typename FnTy>
     void syncRecvApply_ck(uint32_t from_id, Galois::Runtime::RecvBuffer& buf, std::string loopName) {
       auto& net = Galois::Runtime::getSystemNetworkInterface();
       std::string set_timer_str("SYNC_SET_" + loopName + "_" + get_run_identifier());
       std::string doall_str("LAMBDA::REDUCE_RECV_APPLY_" + loopName + "_" + get_run_identifier());
       Galois::StatTimer StatTimer_set(set_timer_str.c_str());
       StatTimer_set.start();

       uint32_t num = masterNodes[from_id].size();
       std::vector<typename FnTy::ValTy> val_vec(num);
       Galois::Runtime::gDeserialize(buf, val_vec);
       if (num > 0) {
         if (!FnTy::reduce_batch(from_id, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), 
                          boost::counting_iterator<uint32_t>(num),
                          [&](uint32_t n) {
                            uint32_t lid = masterNodes[from_id][n];
#ifdef __GALOIS_HET_OPENCL__
                            CLNodeDataWrapper d = clGraph.getDataW(lid);
                            FnTy::reduce(lid, d, val_vec[n]);
#else
                            FnTy::reduce(lid, getData(lid), val_vec[n]);
#endif
                          }, 
                          Galois::loopname(doall_str.c_str()), 
                          Galois::numrun(get_run_identifier()));
         }
       }

       if (net.ID == (from_id + 1) % net.Num) {
        checkpoint_recvBuffer = std::move(buf);
       }
       StatTimer_set.stop();
   }

public:
  typedef typename GraphTy::GraphNode GraphNode;
  typedef typename GraphTy::iterator iterator;
  typedef typename GraphTy::const_iterator const_iterator;
  typedef typename GraphTy::local_iterator local_iterator;
  typedef typename GraphTy::const_local_iterator const_local_iterator;
  typedef typename GraphTy::edge_iterator edge_iterator;


  //hGraph(const std::string& filename, const std::string& partitionFolder, unsigned host, unsigned numHosts, std::vector<unsigned> scalefactor = std::vector<unsigned>()) :
  hGraph(unsigned host, unsigned numHosts) :
      GlobalObject(this), transposed(false), round(false), id(host), 
      numHosts(numHosts), statGhostNodes("TotalGhostNodes") {
    if (useGidMetadata) {
      if (enforce_data_mode != offsetsData) {
        useGidMetadata = false;
      }
    }
#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
    comm_mode = 0;
#endif
#endif
    total_isolatedNodes = 0;
    masterNodes.resize(numHosts);
    mirrorNodes.resize(numHosts);
    //masterNodes_bitvec.resize(numHosts);
    numPipelinedPhases = 0;
    num_recv_expected = 0;
    num_run = 0;
    num_iteration = 0;
    totalEdges = 0;

    //uint32_t numNodes;
    //uint64_t numEdges;
#if 0
    std::string part_fileName = getPartitionFileName(filename,partitionFolder,id,numHosts);
    //OfflineGraph g(part_fileName);
    hGraph(filename, partitionFolder, host, numHosts, scalefactor, numNodes, numOwned, numEdges, totalNodes, id);
    graph.allocateFrom(numNodes, numEdges);
    //std::cerr << "Allocate done\n";

    graph.constructNodes();
    //std::cerr << "Construct nodes done\n";
    loadEdges(graph, g);
    std::cerr << "Edges loaded \n";
    //testPart<PartitionTy>(g);

#ifdef __GALOIS_HET_OPENCL__
    clGraph.load_from_hgraph(*this);
#endif
    setup_communication();


#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifndef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
    simulate_communication();
#endif
#endif
#endif
  }
  void setup_communication() {
    Galois::StatTimer StatTimer_comm_setup("COMMUNICATION_SETUP_TIME");

    // so that all hosts start the timer together
    Galois::Runtime::getHostBarrier().wait(); 
    StatTimer_comm_setup.start();

    // Exchange information for memoization optimization.
    exchange_info_init();

    for (uint32_t h = 0; h < masterNodes.size(); ++h) {
       Galois::do_all(boost::counting_iterator<uint32_t>(0), 
                      boost::counting_iterator<uint32_t>(masterNodes[h].size()),
                      [&](uint32_t n){
                        masterNodes[h][n] = G2L(masterNodes[h][n]);
                      }, 
                      Galois::loopname("MASTER_NODES"), 
                      Galois::numrun(get_run_identifier()));
    }

    for (uint32_t h = 0; h < mirrorNodes.size(); ++h) {
       Galois::do_all(boost::counting_iterator<uint32_t>(0), 
                      boost::counting_iterator<uint32_t>(mirrorNodes[h].size()),
                      [&](uint32_t n){
                        mirrorNodes[h][n] = G2L(mirrorNodes[h][n]);
                      }, 
                      Galois::loopname("MIRROR_NODES"), 
                      Galois::numrun(get_run_identifier()));
    }
    StatTimer_comm_setup.stop();

    for (auto x = 0U; x < masterNodes.size(); ++x) {
      std::string master_nodes_str = "MASTER_NODES_TO_" + std::to_string(x);
      Galois::Statistic StatMasterNodes(master_nodes_str);
      StatMasterNodes += masterNodes[x].size();
    }

    totalMirrorNodes = 0;
    for (auto x = 0U; x < mirrorNodes.size(); ++x) {
      std::string mirror_nodes_str = "MIRROR_NODES_FROM_" + std::to_string(x);
      if(x == id)
        continue;
      Galois::Statistic StatMirrorNodes(mirror_nodes_str);
      StatMirrorNodes += mirrorNodes[x].size();
      totalMirrorNodes += mirrorNodes[x].size();
    }

    send_info_to_host();
  }

#ifdef __GALOIS_SIMULATE_COMMUNICATION__
   void simulate_communication() {
     for (int i = 0; i < 10; ++i) {
     simulate_broadcast("");
     simulate_reduce("");

#ifdef __GALOIS_SIMULATE_BARE_MPI_COMMUNICATION__
     simulate_bare_mpi_broadcast("");
     simulate_bare_mpi_reduce("");
#endif
     }
   }
#endif

#if 0
   template<bool PartTy, typename std::enable_if<!std::integral_constant<bool, PartTy>::value>::type* = nullptr>
   void testPart(OfflineGraph& g){
       std::cout << "False type... edge partition\n";
   }

   template<bool PartTy, typename std::enable_if<std::integral_constant<bool, PartTy>::value>::type* = nullptr>
   void testPart(OfflineGraph& g){
       std::cout << "true type... vertex partition\n";
   }
#endif

#if 0
   template<bool isVoidType, typename std::enable_if<!isVoidType>::type* = nullptr>
   void loadEdges(OfflineGraph & g) {
      fprintf(stderr, "Loading edge-data while creating edges.\n");

      uint64_t cur = 0;
      Galois::Timer timer;
      std::cout <<"["<<id<<"]PRE :: NumSeeks ";
      g.num_seeks();
      g.reset_seek_counters();
      timer.start();
      auto ee = g.edge_begin(gid2host[id].first);
      for (auto n = gid2host[id].first; n < gid2host[id].second; ++n) {
         auto ii = ee;
         ee=g.edge_end(n);
         for (; ii < ee; ++ii) {
            auto gdst = g.getEdgeDst(ii);
            decltype(gdst) ldst = G2L(gdst);
            auto gdata = g.getEdgeData<EdgeTy>(ii);
            graph.constructEdge(cur++, ldst, gdata);
         }
         graph.fixEndEdge(G2L(n), cur);
      }

      timer.stop();
      std::cout <<"["<<id<<"]POST :: NumSeeks ";
      g.num_seeks();
      std::cout << "EdgeLoading time " << timer.get_usec()/1000000.0f << " seconds\n";
   }
   template<bool isVoidType, typename std::enable_if<isVoidType>::type* = nullptr>
   void loadEdges(OfflineGraph & g) {
      fprintf(stderr, "Loading void edge-data while creating edges.\n");
      uint64_t cur = 0;
      for (auto n = gid2host[id].first; n < gid2host[id].second; ++n) {
         for (auto ii = g.edge_begin(n), ee = g.edge_end(n); ii < ee; ++ii) {
            auto gdst = g.getEdgeDst(ii);
            decltype(gdst) ldst = G2L(gdst);
            graph.constructEdge(cur++, ldst);
         }
         graph.fixEndEdge(G2L(n), cur);
      }

   }

#endif

  NodeTy& getData(GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    auto& r = getDataImpl<BSPNode>(N, mflag);
//  auto i =Galois::Runtime::NetworkInterface::ID;
    //std::cerr << i << " " << N << " " <<&r << " " << r.dist_current << "\n";
    return r;
  }

  const NodeTy& getData(GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) const {
    auto& r = getDataImpl<BSPNode>(N, mflag);
//  auto i =Galois::Runtime::NetworkInterface::ID;
    //std::cerr << i << " " << N << " " <<&r << " " << r.dist_current << "\n";
    return r;
  }
  typename GraphTy::edge_data_reference getEdgeData(edge_iterator ni, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    return getEdgeDataImpl<BSPEdge>(ni, mflag);
  }

  GraphNode getEdgeDst(edge_iterator ni) {
    return graph.getEdgeDst(ni);
  }

  edge_iterator edge_begin(GraphNode N) {
    return graph.edge_begin(N, Galois::MethodFlag::UNPROTECTED);
  }

  edge_iterator edge_end(GraphNode N) {
    return graph.edge_end(N);
  }

  size_t size() const {
    return graph.size();
  }

  size_t sizeEdges() const {
    return graph.sizeEdges();
  }

  const_iterator begin() const {
    return graph.begin();
  }

  iterator begin() {
    return graph.begin();
  }

  const_iterator end() const {
    if (transposed) return graph.end();
    return graph.begin() + numOwned;
  }

  iterator end() {
    if (transposed) return graph.end();
    return graph.begin() + numOwned;
  }

  const_iterator ghost_begin() const {
    return end();
  }

  iterator ghost_begin() {
    return end();
  }

  const_iterator ghost_end() const {
    return graph.end();
  }

  iterator ghost_end() {
    return graph.end();
  }

  Galois::Runtime::SpecificRange<boost::counting_iterator<size_t>>&
  allNodesRange() {
    assert(specificRanges.size() == 3);
    return specificRanges[0];
  }

  Galois::Runtime::SpecificRange<boost::counting_iterator<size_t>>&
  masterNodesRange() {
    assert(specificRanges.size() == 3);
    return specificRanges[1];
  }
 
  Galois::Runtime::SpecificRange<boost::counting_iterator<size_t>>&
  allNodesWithEdgesRange() {
    assert(specificRanges.size() == 3);
    return specificRanges[2];
  }

  // DEPRECATED: do not use
  ///** 
  // * Call after graph is completely constructed. Attempts to more evenly 
  // * distribute nodes among threads by checking the number of edges per
  // * node and determining where each thread should start. 
  // * This should only be done once after graph construction to prevent
  // * too much overhead.
  // **/
  //void determine_thread_ranges() {
  //  graph.determineThreadRanges();
  //}

  /**
   * A version of determine_thread_ranges that computes the range offsets
   * for a specific range of the graph. 
   *
   * Note that threadRangesEdge is not determined for this variant, meaning
   * allocateSpecified will not work if you choose to use this variant.
   *
   * @param beginNode Beginning of range
   * @param endNode End of range, non-inclusive
   * @param returnRanges Vector to store thread offsets for ranges in
   */
  void determine_thread_ranges(uint32_t beginNode, uint32_t endNode,
                               std::vector<uint32_t>& returnRanges) {
    graph.determineThreadRanges(beginNode, endNode, returnRanges,
                                nodeAlphaRanges);
  }

  /**
   * A version of determine_thread_ranges that uses a pre-computed prefix sum
   * to determine division of nodes among threads.
   *
   * @param total_nodes The total number of nodes (masters + mirrors) on this
   * partition.
   * @param edge_prefix_sum The edge prefix sum of the nodes on this partition.
   */
  void determine_thread_ranges(uint32_t total_nodes, 
                               std::vector<uint64_t> edge_prefix_sum) {
    // Old way that determined thread ranges with a linear scan.
    //graph.determineThreadRanges(total_nodes, edge_prefix_sum, nodeAlphaRanges);
    //graph.determineThreadRangesEdge(edge_prefix_sum);

    // uses a binary search to find divisions
    graph.determineThreadRangesByNode(edge_prefix_sum);
  }
  
  /**
   * Determine the ranges for the edges of a graph for each thread given the
   * prefix sum of edges. threadRanges must already be calculated.
   *
   * @param edge_prefix_sum Prefix sum of edges 
   */
  void determine_thread_ranges_edge(std::vector<uint64_t> edge_prefix_sum) {
    graph.determineThreadRangesEdge(edge_prefix_sum);
  }


  /** 
   * Determines the thread ranges for master nodes only and saves them to
   * the object.
   *
   * Only call after graph is constructed + only call once
   */
  void determine_thread_ranges_master() {
    // make sure this hasn't been called before
    assert(masterRanges.size() == 0);

    // first check if we even need to do any work; if already calculated,
    // use already calculated vector
    if (beginMaster == 0 && endMaster == numNodes) {
      masterRanges = graph.getThreadRangesVector();
    } else if (beginMaster == 0 && endMaster == numNodesWithEdges &&
               withEdgeRanges.size() != 0) {
      masterRanges = withEdgeRanges;
    } else {
      //printf("Manually det. master thread ranges\n");
      graph.determineThreadRanges(beginMaster, endMaster, masterRanges, 
                                  nodeAlphaRanges);
    }
  }

  /** 
   * Determines the thread ranges for nodes with edges only and saves them to
   * the object.
   *
   * Only call after graph is constructed + only call once
   */
  void determine_thread_ranges_with_edges() {
    // make sure not called before
    assert(withEdgeRanges.size() == 0);

    // first check if we even need to do any work; if already calculated,
    // use already calculated vector
    if (numNodesWithEdges == numNodes) {
      withEdgeRanges = graph.getThreadRangesVector();
    } else if (beginMaster == 0 && endMaster == numNodesWithEdges &&
               masterRanges.size() != 0) {
      withEdgeRanges = masterRanges;
    } else {
      //printf("Manually det. with edges thread ranges\n");
      graph.determineThreadRanges(0, numNodesWithEdges, withEdgeRanges, 
                                  nodeAlphaRanges);
    }
  }

  /**
   *
   */
  void initialize_specific_ranges() {
    assert(specificRanges.size() == 0);

    assert(graph.getThreadRangesVector().size() != 0);
    assert(masterRanges.size() != 0);
    assert(withEdgeRanges.size() != 0);

    // 0 is all nodes
    specificRanges.push_back(
      Galois::Runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(0),
        boost::counting_iterator<size_t>(numNodes),
        graph.getThreadRanges()
      )
    );

    // 1 is master nodes
    specificRanges.push_back(
      Galois::Runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(beginMaster),
        boost::counting_iterator<size_t>(endMaster),
        masterRanges.data()
      )
    );

    // 2 is with edge nodes
    specificRanges.push_back(
      Galois::Runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(0),
        boost::counting_iterator<size_t>(numNodesWithEdges),
        withEdgeRanges.data()
      )
    );

    assert(specificRanges.size() == 3);
  }

  void exchange_info_init(){
    auto& net = Galois::Runtime::getSystemNetworkInterface();

    for (unsigned x = 0; x < net.Num; ++x) {
      if(x == id) continue;

      Galois::Runtime::SendBuffer b;
      gSerialize(b, mirrorNodes[x]);
      net.sendTagged(x, Galois::Runtime::evilPhase, b);
    }

    //receive
    for (unsigned x = 0; x < net.Num; ++x) {
      if(x == id) continue;

      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      do {
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      } while(!p);

      Galois::Runtime::gDeserialize(p->second, masterNodes[p->first]);
    }
    ++Galois::Runtime::evilPhase;
  }

  void send_info_to_host(){
    auto& net = Galois::Runtime::getSystemNetworkInterface();

    //send info to host
    for(unsigned x = 0; x < net.Num; ++x){
      if(x == id) continue;

      Galois::Runtime::SendBuffer b;
      gSerialize(b, totalMirrorNodes, totalOwnedNodes);
      net.sendTagged(x, Galois::Runtime::evilPhase, b);
    }

    //receive
    uint64_t global_total_mirror_nodes = totalMirrorNodes;
    uint64_t global_total_owned_nodes = totalOwnedNodes;

    for(unsigned x = 0; x < net.Num; ++x){
      if(x == id) continue;

      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      do{
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      }while(!p);

      uint64_t total_mirror_nodes_from_others;
      uint64_t total_owned_nodes_from_others;
      Galois::Runtime::gDeserialize(p->second, total_mirror_nodes_from_others, total_owned_nodes_from_others);
      global_total_mirror_nodes += total_mirror_nodes_from_others;
      global_total_owned_nodes += total_owned_nodes_from_others;
    }
    ++Galois::Runtime::evilPhase;

    //print 
    if (net.ID == 0) {
      float replication_factor = (float)(global_total_mirror_nodes + totalNodes)/(float)totalNodes;
      Galois::Runtime::reportStat("(NULL)", "REPLICATION_FACTOR_" + get_run_identifier(), std::to_string(replication_factor), 0);


      float replication_factor_new = (float)(global_total_mirror_nodes + global_total_owned_nodes - total_isolatedNodes)/(float)(global_total_owned_nodes - total_isolatedNodes);
      Galois::Runtime::reportStat("(NULL)", "REPLICATION_FACTOR_NEW_" + get_run_identifier(), std::to_string(replication_factor_new), 0);

      Galois::Runtime::reportStat("(NULL)", "TOTAL_NODES_" + get_run_identifier(), totalNodes, 0);
      Galois::Runtime::reportStat("(NULL)", "TOTAL_OWNED_" + get_run_identifier(), global_total_owned_nodes, 0);
      Galois::Runtime::reportStat("(NULL)", "TOTAL_GLOBAL_GHOSTNODES_" + get_run_identifier(), global_total_mirror_nodes, 0);
      Galois::Runtime::reportStat("(NULL)", "TOTAL_ISOLATED_NODES_" + get_run_identifier(), total_isolatedNodes, 0);
    }
  }

#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_BARE_MPI_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void simulate_bare_mpi_broadcast(std::string loopName, bool mem_copy = false) {
      //std::cerr << "WARNING: requires MPI_THREAD_MULTIPLE to be set in MPI_Init_thread() and Net to not receive MPI messages with tag 32767\n";
      std::string statSendBytes_str("SIMULATE_MPI_SEND_BYTES_BROADCAST_" + loopName + "_" + std::to_string(num_run));
      Galois::Statistic SyncBroadcast_send_bytes(statSendBytes_str);
      std::string timer_str("SIMULATE_MPI_BROADCAST_" + loopName + "_" + std::to_string(num_run));
      Galois::StatTimer StatTimer_syncBroadcast(timer_str.c_str());
      std::string timer_barrier_str("SIMULATE_MPI_BROADCAST_BARRIER_" + loopName + "_" + std::to_string(num_run));
      Galois::StatTimer StatTimerBarrier_syncBroadcast(timer_barrier_str.c_str());
      std::string extract_timer_str("SIMULATE_MPI_BROADCAST_EXTRACT_" + loopName +"_" + std::to_string(num_run));
      Galois::StatTimer StatTimer_extract(extract_timer_str.c_str());
      std::string set_timer_str("SIMULATE_MPI_BROADCAST_SET_" + loopName +"_" + std::to_string(num_run));
      Galois::StatTimer StatTimer_set(set_timer_str.c_str());

#ifndef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      MPI_Barrier(MPI_COMM_WORLD);
#endif
      StatTimer_syncBroadcast.start();
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      std::vector<MPI_Request> requests(2 * net.Num);
      unsigned num_requests = 0;

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      static std::vector< std::vector<typename FnTy::ValTy> > sb;
#else
      static std::vector< std::vector<uint64_t> > sb;
#endif
      sb.resize(net.Num);
      std::vector<uint8_t> bs[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = masterNodes[x].size();
         if((x == id) || (num == 0))
           continue;

         StatTimer_extract.start();

         sb[x].resize(num);

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
         std::vector<typename FnTy::ValTy> &val_vec = sb[x];
#else
         size_t size = num * sizeof(uint64_t);
         std::vector<uint64_t> &val_vec = sb[x];
#endif

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::extract_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
               uint32_t localID = masterNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
               auto val = FnTy::extract((localID), clGraph.getDataR((localID)));
#else
               auto val = FnTy::extract((localID), getData(localID));
#endif
               val_vec[n] = val;

               }, Galois::loopname("BROADCAST_EXTRACT"), Galois::numrun(get_run_identifier()));
         }
#else
         val_vec[0] = 1;
#endif
         
         if (mem_copy) {
           bs[x].resize(size);
           memcpy(bs[x].data(), sb[x].data(), size);
         }

         StatTimer_extract.stop();

         SyncBroadcast_send_bytes += size;
         //std::cerr << "[" << id << "]" << " mpi send to " << x << " : " << size << "\n";
         if (mem_copy)
           MPI_Isend((uint8_t *)bs[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
         else
           MPI_Isend((uint8_t *)sb[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      static std::vector< std::vector<typename FnTy::ValTy> > rb;
#else
      static std::vector< std::vector<uint64_t> > rb;
#endif
      rb.resize(net.Num);
      std::vector<uint8_t> b[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = mirrorNodes[x].size();
         if((x == id) || (num == 0))
           continue;
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
#else
         size_t size = num * sizeof(uint64_t);
#endif
         rb[x].resize(num);
         if (mem_copy) b[x].resize(size);

         //std::cerr << "[" << id << "]" << " mpi receive from " << x << " : " << size << "\n";
         if (mem_copy)
           MPI_Irecv((uint8_t *)b[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
         else
           MPI_Irecv((uint8_t *)rb[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

      StatTimerBarrier_syncBroadcast.start();
      MPI_Waitall(num_requests, &requests[0], MPI_STATUSES_IGNORE);
      StatTimerBarrier_syncBroadcast.stop();

      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = mirrorNodes[x].size();
         if((x == id) || (num == 0))
           continue;

         StatTimer_set.start();

         //std::cerr << "[" << id << "]" << " mpi received from " << x << "\n";
         if (mem_copy) memcpy(rb[x].data(), b[x].data(), b[x].size());
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         std::vector<typename FnTy::ValTy> &val_vec = rb[x];
#else
         std::vector<uint64_t> &val_vec = rb[x];
#endif
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::setVal_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
               uint32_t localID = mirrorNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
               {
               CLNodeDataWrapper d = clGraph.getDataW(localID);
               FnTy::setVal(localID, d, val_vec[n]);
               }
#else
               FnTy::setVal(localID, getData(localID), val_vec[n]);
#endif
               }, Galois::loopname("BROADCAST_SET"), Galois::numrun(get_run_identifier()));
          }
#endif

         StatTimer_set.stop();
      }

      //std::cerr << "[" << id << "]" << "pull mpi done\n";
      StatTimer_syncBroadcast.stop();
   }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void simulate_bare_mpi_reduce(std::string loopName, bool mem_copy = false) {
      //std::cerr << "WARNING: requires MPI_THREAD_MULTIPLE to be set in MPI_Init_thread() and Net to not receive MPI messages with tag 32767\n";
      std::string statSendBytes_str("SIMULATE_MPI_SEND_BYTES_REDUCE_" + loopName + "_" + std::to_string(num_run));
      Galois::Statistic SyncReduce_send_bytes(statSendBytes_str);
      std::string timer_str("SIMULATE_MPI_REDUCE_" + loopName + "_" + std::to_string(num_run));
      Galois::StatTimer StatTimer_syncReduce(timer_str.c_str());
      std::string timer_barrier_str("SIMULATE_MPI_REDUCE_BARRIER_" + loopName + "_" + std::to_string(num_run));
      Galois::StatTimer StatTimerBarrier_syncReduce(timer_barrier_str.c_str());
      std::string extract_timer_str("SIMULATE_MPI_REDUCE_EXTRACT_" + loopName +"_" + std::to_string(num_run));
      Galois::StatTimer StatTimer_extract(extract_timer_str.c_str());
      std::string set_timer_str("SIMULATE_MPI_REDUCE_SET_" + loopName +"_" + std::to_string(num_run));
      Galois::StatTimer StatTimer_set(set_timer_str.c_str());

#ifndef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      MPI_Barrier(MPI_COMM_WORLD);
#endif
      StatTimer_syncReduce.start();
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      std::vector<MPI_Request> requests(2 * net.Num);
      unsigned num_requests = 0;

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      static std::vector< std::vector<typename FnTy::ValTy> > sb;
#else
      static std::vector< std::vector<uint64_t> > sb;
#endif
      sb.resize(net.Num);
      std::vector<uint8_t> bs[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = mirrorNodes[x].size();
         if((x == id) || (num == 0))
           continue;

         StatTimer_extract.start();

         sb[x].resize(num);

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
         std::vector<typename FnTy::ValTy> &val_vec = sb[x];
#else
         size_t size = num * sizeof(uint64_t);
         std::vector<uint64_t> &val_vec = sb[x];
#endif

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::extract_reset_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
                uint32_t lid = mirrorNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
                CLNodeDataWrapper d = clGraph.getDataW(lid);
                auto val = FnTy::extract(lid, getData(lid, d));
                FnTy::reset(lid, d);
#else
                auto val = FnTy::extract(lid, getData(lid));
                FnTy::reset(lid, getData(lid));
#endif
                val_vec[n] = val;
               }, Galois::loopname("REDUCE_EXTRACT"), Galois::numrun(get_run_identifier()));
         }
#else
         val_vec[0] = 1;
#endif
         
         if (mem_copy) {
           bs[x].resize(size);
           memcpy(bs[x].data(), sb[x].data(), size);
         }

         StatTimer_extract.stop();

         SyncReduce_send_bytes += size;
         //std::cerr << "[" << id << "]" << " mpi send to " << x << " : " << size << "\n";
         if (mem_copy)
           MPI_Isend((uint8_t *)bs[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
         else
           MPI_Isend((uint8_t *)sb[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      static std::vector< std::vector<typename FnTy::ValTy> > rb;
#else
      static std::vector< std::vector<uint64_t> > rb;
#endif
      rb.resize(net.Num);
      std::vector<uint8_t> b[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = masterNodes[x].size();
         if((x == id) || (num == 0))
           continue;
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
#else
         size_t size = num * sizeof(uint64_t);
#endif
         rb[x].resize(num);
         if (mem_copy) b[x].resize(size);

         //std::cerr << "[" << id << "]" << " mpi receive from " << x << " : " << size << "\n";
         if (mem_copy)
           MPI_Irecv((uint8_t *)b[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
         else
           MPI_Irecv((uint8_t *)rb[x].data(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

      StatTimerBarrier_syncReduce.start();
      MPI_Waitall(num_requests, &requests[0], MPI_STATUSES_IGNORE);
      StatTimerBarrier_syncReduce.stop();

      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = masterNodes[x].size();
         if((x == id) || (num == 0))
           continue;

         StatTimer_set.start();

         //std::cerr << "[" << id << "]" << " mpi received from " << x << "\n";
         if (mem_copy) memcpy(rb[x].data(), b[x].data(), b[x].size());
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         std::vector<typename FnTy::ValTy> &val_vec = rb[x];
#else
         std::vector<uint64_t> &val_vec = rb[x];
#endif
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::reduce_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num),
               [&](uint32_t n){
               uint32_t lid = masterNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
           CLNodeDataWrapper d = clGraph.getDataW(lid);
           FnTy::reduce(lid, d, val_vec[n]);
#else
           FnTy::reduce(lid, getData(lid), val_vec[n]);
#endif
               }, Galois::loopname("REDUCE_SET"), Galois::numrun(get_run_identifier()));
         }
#endif

         StatTimer_set.stop();
      }
      
      //std::cerr << "[" << id << "]" << "push mpi done\n";
      StatTimer_syncReduce.stop();
   }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void simulate_bare_mpi_broadcast_serialized() {
      //std::cerr << "WARNING: requires MPI_THREAD_MULTIPLE to be set in MPI_Init_thread() and Net to not receive MPI messages with tag 32767\n";
      Galois::StatTimer StatTimer_syncBroadcast("SIMULATE_MPI_BROADCAST");
      Galois::Statistic SyncBroadcast_send_bytes("SIMULATE_MPI_BROADCAST_SEND_BYTES");

#ifndef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      MPI_Barrier(MPI_COMM_WORLD);
#endif
      StatTimer_syncBroadcast.start();
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      std::vector<MPI_Request> requests(2 * net.Num);
      unsigned num_requests = 0;

      Galois::Runtime::SendBuffer sb[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = masterNodes[x].size();
         if((x == id) || (num == 0))
           continue;

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
         std::vector<typename FnTy::ValTy> val_vec(num);
#else
         size_t size = num * sizeof(uint64_t);
         std::vector<uint64_t> val_vec(num);
#endif
         size+=8;

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::extract_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
               uint32_t localID = masterNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
               auto val = FnTy::extract((localID), clGraph.getDataR((localID)));
#else
               auto val = FnTy::extract((localID), getData(localID));
#endif
               val_vec[n] = val;

               }, Galois::loopname("BROADCAST_EXTRACT"), Galois::numrun(get_run_identifier()));
         }
#else
         val_vec[0] = 1;
#endif

         Galois::Runtime::gSerialize(sb[x], val_vec);
         assert(size == sb[x].size());
         
         SyncBroadcast_send_bytes += size;
         //std::cerr << "[" << id << "]" << " mpi send to " << x << " : " << size << "\n";
         MPI_Isend(sb[x].linearData(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

      Galois::Runtime::RecvBuffer rb[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = mirrorNodes[x].size();
         if((x == id) || (num == 0))
           continue;
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
#else
         size_t size = num * sizeof(uint64_t);
#endif
         size+=8;
         rb[x].reset(size);

         //std::cerr << "[" << id << "]" << " mpi receive from " << x << " : " << size << "\n";
         MPI_Irecv((uint8_t *)rb[x].linearData(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

      MPI_Waitall(num_requests, &requests[0], MPI_STATUSES_IGNORE);

      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = mirrorNodes[x].size();
         if((x == id) || (num == 0))
           continue;
         //std::cerr << "[" << id << "]" << " mpi received from " << x << "\n";
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         std::vector<typename FnTy::ValTy> val_vec(num);
#else
         std::vector<uint64_t> val_vec(num);
#endif
         Galois::Runtime::gDeserialize(rb[x], val_vec);
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::setVal_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
               uint32_t localID = mirrorNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
               {
               CLNodeDataWrapper d = clGraph.getDataW(localID);
               FnTy::setVal(localID, d, val_vec[n]);
               }
#else
               FnTy::setVal(localID, getData(localID), val_vec[n]);
#endif
               }, Galois::loopname("BROADCAST_SET"), Galois::numrun(get_run_identifier()));
          }
#endif
      }

      //std::cerr << "[" << id << "]" << "pull mpi done\n";
      StatTimer_syncBroadcast.stop();
   }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void simulate_bare_mpi_reduce_serialized() {
      //std::cerr << "WARNING: requires MPI_THREAD_MULTIPLE to be set in MPI_Init_thread() and Net to not receive MPI messages with tag 32767\n";
      Galois::StatTimer StatTimer_syncReduce("SIMULATE_MPI_REDUCE");
      Galois::Statistic SyncReduce_send_bytes("SIMULATE_MPI_REDUCE_SEND_BYTES");

#ifndef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      MPI_Barrier(MPI_COMM_WORLD);
#endif
      StatTimer_syncReduce.start();
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      std::vector<MPI_Request> requests(2 * net.Num);
      unsigned num_requests = 0;

      Galois::Runtime::SendBuffer sb[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = mirrorNodes[x].size();
         if((x == id) || (num == 0))
           continue;

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
         std::vector<typename FnTy::ValTy> val_vec(num);
#else
         size_t size = num * sizeof(uint64_t);
         std::vector<uint64_t> val_vec(num);
#endif
         size+=8;

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::extract_reset_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
                uint32_t lid = mirrorNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
                CLNodeDataWrapper d = clGraph.getDataW(lid);
                auto val = FnTy::extract(lid, getData(lid, d));
                FnTy::reset(lid, d);
#else
                auto val = FnTy::extract(lid, getData(lid));
                FnTy::reset(lid, getData(lid));
#endif
                val_vec[n] = val;
               }, Galois::loopname("REDUCE_EXTRACT"), Galois::numrun(get_run_identifier()));
         }
#else
         val_vec[0] = 1;
#endif

         Galois::Runtime::gSerialize(sb[x], val_vec);
         assert(size == sb[x].size());

         SyncReduce_send_bytes += size;
         //std::cerr << "[" << id << "]" << " mpi send to " << x << " : " << size << "\n";
         MPI_Isend(sb[x].linearData(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

      Galois::Runtime::RecvBuffer rb[net.Num];
      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = masterNodes[x].size();
         if((x == id) || (num == 0))
           continue;
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         size_t size = num * sizeof(typename FnTy::ValTy);
#else
         size_t size = num * sizeof(uint64_t);
#endif
         size+=8;
         rb[x].reset(size);

         //std::cerr << "[" << id << "]" << " mpi receive from " << x << " : " << size << "\n";
         MPI_Irecv((uint8_t *)rb[x].linearData(), size, MPI_BYTE, x, 32767, MPI_COMM_WORLD, &requests[num_requests++]);
      }

      MPI_Waitall(num_requests, &requests[0], MPI_STATUSES_IGNORE);

      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = masterNodes[x].size();
         if((x == id) || (num == 0))
           continue;
         //std::cerr << "[" << id << "]" << " mpi received from " << x << "\n";
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         std::vector<typename FnTy::ValTy> val_vec(num);
#else
         std::vector<uint64_t> val_vec(num);
#endif
         Galois::Runtime::gDeserialize(rb[x], val_vec);
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::reduce_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num),
               [&](uint32_t n){
               uint32_t lid = masterNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
           CLNodeDataWrapper d = clGraph.getDataW(lid);
           FnTy::reduce(lid, d, val_vec[n]);
#else
           FnTy::reduce(lid, getData(lid), val_vec[n]);
#endif
               }, Galois::loopname("REDUCE_SET"), Galois::numrun(get_run_identifier()));
         }
#endif
      }
      
      //std::cerr << "[" << id << "]" << "push mpi done\n";
      StatTimer_syncReduce.stop();
   }
#endif
#endif

#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void syncRecvApplyPull(Galois::Runtime::RecvBuffer& buf) {
     unsigned from_id;
     uint32_t num;
     std::string loopName;
     Galois::Runtime::gDeserialize(buf, from_id, num);
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
     std::vector<typename FnTy::ValTy> val_vec(num);
#else
     std::vector<uint64_t> val_vec(num);
#endif
     Galois::Runtime::gDeserialize(buf, val_vec);
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
     if (!FnTy::setVal_batch(from_id, val_vec.data())) {
       Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
           uint32_t localID = mirrorNodes[from_id][n];
#ifdef __GALOIS_HET_OPENCL__
           {
           CLNodeDataWrapper d = clGraph.getDataW(localID);
           FnTy::setVal(localID, d, val_vec[n]);
           }
#else
           FnTy::setVal(localID, getData(localID), val_vec[n]);
#endif
           }, Galois::loopname("BROADCAST_SET"), Galois::numrun(get_run_identifier()));
      }
#endif
   }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void syncRecvApplyPush(Galois::Runtime::RecvBuffer& buf) {
     unsigned from_id;
     uint32_t num;
     std::string loopName;
     Galois::Runtime::gDeserialize(buf, from_id, num);
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
     std::vector<typename FnTy::ValTy> val_vec(num);
#else
     std::vector<uint64_t> val_vec(num);
#endif
     Galois::Runtime::gDeserialize(buf, val_vec);
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
     if (!FnTy::reduce_batch(from_id, val_vec.data())) {
       Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num),
           [&](uint32_t n){
           uint32_t lid = masterNodes[from_id][n];
#ifdef __GALOIS_HET_OPENCL__
       CLNodeDataWrapper d = clGraph.getDataW(lid);
       FnTy::reduce(lid, d, val_vec[n]);
#else
       FnTy::reduce(lid, getData(lid), val_vec[n]);
#endif
           }, Galois::loopname("REDUCE_SET"), Galois::numrun(get_run_identifier()));
     }
#endif
   }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void simulate_broadcast(std::string loopName) {
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      void (hGraph::*fn)(Galois::Runtime::RecvBuffer&) = &hGraph::syncRecvApplyPull<FnTy>;
#else
      void (hGraph::*fn)(Galois::Runtime::RecvBuffer&) = &hGraph::syncRecvApplyPull;
#endif
      Galois::StatTimer StatTimer_syncBroadcast("SIMULATE_NET_BROADCAST");
      Galois::Statistic SyncBroadcast_send_bytes("SIMULATE_NET_BROADCAST_SEND_BYTES");

#ifndef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      Galois::Runtime::getHostBarrier().wait();
#endif
      StatTimer_syncBroadcast.start();
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = masterNodes[x].size();
         if((x == id) || (num == 0))
           continue;

         Galois::Runtime::SendBuffer b;
         gSerialize(b, idForSelf(), fn, net.ID, num);

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         std::vector<typename FnTy::ValTy> val_vec(num);
#else
         std::vector<uint64_t> val_vec(num);
#endif

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::extract_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
               uint32_t localID = masterNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
               auto val = FnTy::extract((localID), clGraph.getDataR((localID)));
#else
               auto val = FnTy::extract((localID), getData(localID));
#endif
               val_vec[n] = val;

               }, Galois::loopname("BROADCAST_EXTRACT"), Galois::numrun(get_run_identifier()));
         }
#else
         val_vec[0] = 1;
#endif

         gSerialize(b, val_vec);

         SyncBroadcast_send_bytes += b.size();
         net.sendMsg(x, syncRecv, b);
      }
      //Will force all messages to be processed before continuing
      net.flush();

      Galois::Runtime::getHostBarrier().wait();
      StatTimer_syncBroadcast.stop();
   }

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
   template<typename FnTy>
#endif
   void simulate_reduce(std::string loopName) {
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      void (hGraph::*fn)(Galois::Runtime::RecvBuffer&) = &hGraph::syncRecvApplyPush<FnTy>;
#else
      void (hGraph::*fn)(Galois::Runtime::RecvBuffer&) = &hGraph::syncRecvApplyPush;
#endif
      Galois::StatTimer StatTimer_syncReduce("SIMULATE_NET_REDUCE");
      Galois::Statistic SyncReduce_send_bytes("SIMULATE_NET_REDUCE_SEND_BYTES");

#ifndef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      Galois::Runtime::getHostBarrier().wait();
#endif
      StatTimer_syncReduce.start();
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      for (unsigned x = 0; x < net.Num; ++x) {
         uint32_t num = mirrorNodes[x].size();
         if((x == id) || (num == 0))
           continue;

         Galois::Runtime::SendBuffer b;
         gSerialize(b, idForSelf(), fn, net.ID, num);

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         std::vector<typename FnTy::ValTy> val_vec(num);
#else
         std::vector<uint64_t> val_vec(num);
#endif

#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
         if (!FnTy::extract_reset_batch(x, val_vec.data())) {
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
                uint32_t lid = mirrorNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
                CLNodeDataWrapper d = clGraph.getDataW(lid);
                auto val = FnTy::extract(lid, getData(lid, d));
                FnTy::reset(lid, d);
#else
                auto val = FnTy::extract(lid, getData(lid));
                FnTy::reset(lid, getData(lid));
#endif
                val_vec[n] = val;
               }, Galois::loopname("REDUCE_EXTRACT"), Galois::numrun(get_run_identifier()));
         }
#else
         val_vec[0] = 1;
#endif

         gSerialize(b, val_vec);

         SyncReduce_send_bytes += b.size();
         net.sendMsg(x, syncRecv, b);
      }
      //Will force all messages to be processed before continuing
      net.flush();

      Galois::Runtime::getHostBarrier().wait();

      StatTimer_syncReduce.stop();
   }
#endif

private:

   template<SyncType syncType>
   void get_offsets_from_bitset(const std::string &loopName, const Galois::DynamicBitSet &bitset_comm, std::vector<unsigned int> &offsets, size_t &bit_set_count) {
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     std::string offsets_timer_str(syncTypeStr + "_OFFSETS_" + loopName + "_" + get_run_identifier());
     Galois::StatTimer StatTimer_offsets(offsets_timer_str.c_str());
     StatTimer_offsets.start();
     auto activeThreads = Galois::getActiveThreads();
     std::vector<unsigned int> t_prefix_bit_counts(activeThreads);
     Galois::on_each([&](unsigned tid, unsigned nthreads) {
         unsigned int block_size = bitset_comm.size() / nthreads;
         if ((bitset_comm.size() % nthreads) > 0) ++block_size;
         assert((block_size * nthreads) >= bitset_comm.size());
         unsigned int start = tid*block_size;
         unsigned int end = (tid+1)*block_size;
         if (end > bitset_comm.size()) end = bitset_comm.size();
         unsigned int count = 0;
         for (unsigned int i = start; i < end; ++i) {
           if (bitset_comm.test(i)) ++count;
         }
         t_prefix_bit_counts[tid] = count;
     });
     for (unsigned int i = 1; i < activeThreads; ++i) {
       t_prefix_bit_counts[i] += t_prefix_bit_counts[i-1];
     }
     bit_set_count = t_prefix_bit_counts[activeThreads - 1];
     if (bit_set_count > 0) {
       offsets.resize(bit_set_count);
       Galois::on_each([&](unsigned tid, unsigned nthreads) {
           unsigned int block_size = bitset_comm.size() / nthreads;
           if ((bitset_comm.size() % nthreads) > 0) ++block_size;
           assert((block_size * nthreads) >= bitset_comm.size());
           unsigned int start = tid*block_size;
           unsigned int end = (tid+1)*block_size;
           if (end > bitset_comm.size()) end = bitset_comm.size();
           unsigned int count = 0;
           unsigned int t_prefix_bit_count;
           if (tid == 0) t_prefix_bit_count = 0;
           else t_prefix_bit_count = t_prefix_bit_counts[tid-1];
           for (unsigned int i = start; i < end; ++i) {
             if (bitset_comm.test(i)) {
               offsets[t_prefix_bit_count + count] = i;
               ++count;
             }
           }
       });
     }
     StatTimer_offsets.stop();
   }

   template<typename FnTy, SyncType syncType>
   void get_bitset_and_offsets(const std::string &loopName, 
                               const std::vector<size_t> &indices, 
                               const Galois::DynamicBitSet &bitset_compute, 
                               Galois::DynamicBitSet &bitset_comm, 
                               std::vector<unsigned int> &offsets, 
                               size_t &bit_set_count, DataCommMode &data_mode) {
     if (enforce_data_mode != onlyData) {
       bitset_comm.reset();
       std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
       std::string doall_str(syncTypeStr + "_BITSET_" + loopName);

       Galois::do_all(boost::counting_iterator<unsigned int>(0), 
                      boost::counting_iterator<unsigned int>(indices.size()), 
                      [&](unsigned int n) {
                        size_t lid = indices[n];
                        if (bitset_compute.test(lid)) {
                          bitset_comm.set(n);
                        }
                      }, 
                      Galois::loopname(doall_str.c_str()), 
                      Galois::numrun(get_run_identifier()));

       get_offsets_from_bitset<syncType>(loopName, bitset_comm, offsets, 
                                         bit_set_count);
     }

     if (enforce_data_mode != noData) {
       data_mode = enforce_data_mode;
     } else { // auto
       if (bit_set_count == 0) {
         data_mode = noData;
       } else if ((bit_set_count * sizeof(unsigned int)) < 
                    bitset_comm.alloc_size()) {
         data_mode = offsetsData;
       } else if ((bit_set_count * sizeof(typename FnTy::ValTy) + bitset_comm.alloc_size()) < 
                   (indices.size() * sizeof(typename FnTy::ValTy))) {
         data_mode = bitsetData;
       } else {
         data_mode = onlyData;
       }
     }
   }

   /* Reduction extract resets the value afterwards */
   template<typename FnTy, SyncType syncType, 
            typename std::enable_if<syncType == syncReduce>::type* = nullptr>
   typename FnTy::ValTy extract_wrapper(size_t lid) {
#ifdef __GALOIS_HET_OPENCL__
     CLNodeDataWrapper d = clGraph.getDataW(lid);
     auto val = FnTy::extract(lid, getData(lid, d));
     FnTy::reset(lid, d);
#else
     auto val = FnTy::extract(lid, getData(lid));
     FnTy::reset(lid, getData(lid));
#endif
     return val;
   }

   /* Broadcast extract doesn't reset the value */
   template<typename FnTy, SyncType syncType, 
            typename std::enable_if<syncType == syncBroadcast>::type* = nullptr>
   typename FnTy::ValTy  extract_wrapper(size_t lid) {
#ifdef __GALOIS_HET_OPENCL__
     CLNodeDataWrapper d = clGraph.getDataW(lid);
     return FnTy::extract(lid, getData(lid, d));
#else
     return FnTy::extract(lid, getData(lid));
#endif
   }

  template<typename FnTy, SyncType syncType, bool identity_offsets = false, bool parallelize = true>
  void extract_subset(const std::string &loopName, 
                      const std::vector<size_t> &indices, size_t size, 
                      const std::vector<unsigned int> &offsets,
                      std::vector<typename FnTy::ValTy> &val_vec, 
                      size_t start = 0) {
     if (parallelize) {
       std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
       std::string doall_str(syncTypeStr + "_EXTRACTVAL_" + loopName);
       Galois::do_all(boost::counting_iterator<unsigned int>(start), 
                      boost::counting_iterator<unsigned int>(start + size), 
                      [&](unsigned int n){
                        unsigned int offset;
                        if (identity_offsets) offset = n;
                        else offset = offsets[n];
                        size_t lid = indices[offset];
                        val_vec[n - start] = extract_wrapper<FnTy, syncType>(lid);
                      }, 
                      Galois::loopname(doall_str.c_str()), 
                      Galois::numrun(get_run_identifier()));
     } else {
       for (unsigned n = start; n < start + size; ++n) {
          unsigned int offset;
          if (identity_offsets) offset = n;
          else offset = offsets[n];
          size_t lid = indices[offset];
          val_vec[n - start] = extract_wrapper<FnTy, syncType>(lid);
       }
     }
   }
    
  template<typename FnTy, typename SeqTy, SyncType syncType, bool identity_offsets = false, bool parallelize = true>
   void extract_subset(const std::string &loopName, 
                       const std::vector<size_t> &indices, size_t size, 
                       const std::vector<unsigned int> &offsets, 
                       Galois::Runtime::SendBuffer& b, SeqTy lseq, 
                       size_t start = 0) {
     if (parallelize) {
       std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
       std::string doall_str(syncTypeStr + "_EXTRACTVAL_" + loopName);
       Galois::do_all(boost::counting_iterator<unsigned int>(start), boost::counting_iterator<unsigned int>(start + size), [&](unsigned int n){
          unsigned int offset;
          if (identity_offsets) offset = n;
          else offset = offsets[n];
          size_t lid = indices[offset];
          gSerializeLazy(b, lseq, n-start, extract_wrapper<FnTy, syncType>(lid));
       }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
     } else {
       for (unsigned int n = start; n < start + size; ++n) {
          unsigned int offset;
          if (identity_offsets) offset = n;
          else offset = offsets[n];
          size_t lid = indices[offset];
          gSerializeLazy(b, lseq, n-start, extract_wrapper<FnTy, syncType>(lid));
       }
     }
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncReduce>::type* = nullptr>
   bool extract_batch_wrapper(unsigned x, std::vector<typename FnTy::ValTy> &v) {
     return FnTy::extract_reset_batch(x, v.data());
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncBroadcast>::type* = nullptr>
   bool extract_batch_wrapper(unsigned x, std::vector<typename FnTy::ValTy> &v) {
     return FnTy::extract_batch(x, v.data());
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncReduce>::type* = nullptr>
   bool extract_batch_wrapper(unsigned x, Galois::DynamicBitSet &b, std::vector<unsigned int> &o, std::vector<typename FnTy::ValTy> &v, size_t &s, DataCommMode& data_mode) {
     return FnTy::extract_reset_batch(x, (unsigned long long int *)b.get_vec().data(), o.data(), v.data(), &s, &data_mode);
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncBroadcast>::type* = nullptr>
   bool extract_batch_wrapper(unsigned x, Galois::DynamicBitSet &b, std::vector<unsigned int> &o, std::vector<typename FnTy::ValTy> &v, size_t &s, DataCommMode& data_mode) {
     return FnTy::extract_batch(x, (unsigned long long int *)b.get_vec().data(), o.data(), v.data(), &s, &data_mode);
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncReduce>::type* = nullptr>
   void set_wrapper(size_t lid, typename FnTy::ValTy val, Galois::DynamicBitSet& bit_set_compute) {
#ifdef __GALOIS_HET_OPENCL__
     CLNodeDataWrapper d = clGraph.getDataW(lid);
     FnTy::reduce(lid, d, val);
#else
     if (FnTy::reduce(lid, getData(lid), val)) {
       if (bit_set_compute.size() != 0) bit_set_compute.set(lid);
     }
#endif
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncBroadcast>::type* = nullptr>
   void set_wrapper(size_t lid, typename FnTy::ValTy val, Galois::DynamicBitSet& bit_set_compute) {
#ifdef __GALOIS_HET_OPENCL__
     CLNodeDataWrapper d = clGraph.getDataW(lid);
     FnTy::setVal(lid, d, val_vec[n]);
#else
     FnTy::setVal(lid, getData(lid), val);
#endif
   }

   template<typename FnTy, SyncType syncType, bool identity_offsets = false, bool parallelize = true>
   void set_subset(const std::string &loopName, const std::vector<size_t> &indices, size_t size, const std::vector<unsigned int> &offsets, std::vector<typename FnTy::ValTy> &val_vec, Galois::DynamicBitSet& bit_set_compute, size_t start = 0) {
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     std::string doall_str(syncTypeStr + "_SETVAL_" + loopName + "_" + get_run_identifier());
     if (parallelize) {
       Galois::do_all(boost::counting_iterator<unsigned int>(start), boost::counting_iterator<unsigned int>(start + size), [&](unsigned int n){
          unsigned int offset;
          if (identity_offsets) offset = n;
          else offset = offsets[n];
          size_t lid = indices[offset];
          set_wrapper<FnTy, syncType>(lid, val_vec[n - start], bit_set_compute);
       }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
     } else {
       for (unsigned int n = start; n < start + size; ++n) {
          unsigned int offset;
          if (identity_offsets) offset = n;
          else offset = offsets[n];
          size_t lid = indices[offset];
          set_wrapper<FnTy, syncType>(lid, val_vec[n - start], bit_set_compute);
       }
     }
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncReduce>::type* = nullptr>
   bool set_batch_wrapper(unsigned x, std::vector<typename FnTy::ValTy> &v) {
     return FnTy::reduce_batch(x, v.data());
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncBroadcast>::type* = nullptr>
   bool set_batch_wrapper(unsigned x, std::vector<typename FnTy::ValTy> &v) {
     return FnTy::setVal_batch(x, v.data());
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncReduce>::type* = nullptr>
   bool set_batch_wrapper(unsigned x, Galois::DynamicBitSet &b, std::vector<unsigned int> &o, std::vector<typename FnTy::ValTy> &v, size_t &s, DataCommMode& data_mode) {
     return FnTy::reduce_batch(x, (unsigned long long int *)b.get_vec().data(), o.data(), v.data(), s, data_mode);
   }

   template<typename FnTy, SyncType syncType, typename std::enable_if<syncType == syncBroadcast>::type* = nullptr>
   bool set_batch_wrapper(unsigned x, Galois::DynamicBitSet &b, std::vector<unsigned int> &o, std::vector<typename FnTy::ValTy> &v, size_t &s, DataCommMode& data_mode) {
     return FnTy::setVal_batch(x, (unsigned long long int *)b.get_vec().data(), o.data(), v.data(), s, data_mode);
   }

   template<SyncType syncType>
   void convert_lid_to_gid(const std::string &loopName, std::vector<unsigned int> &offsets) {
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     std::string doall_str(syncTypeStr + "_LID2GID_" + loopName + "_" + get_run_identifier());
     Galois::do_all(boost::counting_iterator<unsigned int>(0), boost::counting_iterator<unsigned int>(offsets.size()), [&](unsigned int n){
         offsets[n] = static_cast<uint32_t>(getGID(offsets[n]));
     }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
   }

   template<SyncType syncType>
   void convert_gid_to_lid(const std::string &loopName, std::vector<unsigned int> &offsets) {
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     std::string doall_str(syncTypeStr + "_GID2LID_" + loopName + "_" + get_run_identifier());
     Galois::do_all(boost::counting_iterator<unsigned int>(0), boost::counting_iterator<unsigned int>(offsets.size()), [&](unsigned int n){
         offsets[n] = static_cast<uint32_t>(getLID(offsets[n]));
     }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
   }

   template<SyncType syncType, typename SyncFnTy>
   void sync_extract(std::string loopName, unsigned from_id, std::vector<size_t> &indices, Galois::Runtime::SendBuffer &b) {
     uint32_t num = indices.size();
     static std::vector<typename SyncFnTy::ValTy> val_vec; //sometimes wasteful
     static std::vector<unsigned int> offsets;
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     std::string extract_timer_str(syncTypeStr + "_EXTRACT_" + loopName +"_" + get_run_identifier());
     Galois::StatTimer StatTimer_extract(extract_timer_str.c_str());
     StatTimer_extract.start();
     DataCommMode data_mode;

     if (num > 0) {
       data_mode = onlyData;
       val_vec.resize(num);

       bool batch_succeeded = extract_batch_wrapper<SyncFnTy, syncType>(from_id, val_vec);

       if (!batch_succeeded) {
         gSerialize(b, onlyData);
         auto lseq = gSerializeLazySeq(b, num, (std::vector<typename SyncFnTy::ValTy>*)nullptr);
         extract_subset<SyncFnTy, decltype(lseq), syncType, true, true>(loopName, indices, num, offsets, b, lseq);
       } else {
         gSerialize(b, onlyData, val_vec);
       }
     } else {
       data_mode = noData;
       gSerialize(b, noData);
     }

     StatTimer_extract.stop();
     std::string metadata_str(syncTypeStr + "_METADATA_MODE" + std::to_string(data_mode) + "_" + loopName + "_" + get_run_identifier());
     Galois::Statistic Statistic_metadata(metadata_str.c_str());
     Statistic_metadata += 1;
   }

   // Bitset variant (uses bitset to determine what to sync)
   template<SyncType syncType, typename SyncFnTy, typename BitsetFnTy>
   void sync_extract(std::string loopName, unsigned from_id, 
                     std::vector<size_t> &indices, 
                     Galois::Runtime::SendBuffer &b) {
     uint32_t num = indices.size();
     static Galois::DynamicBitSet bit_set_comm;
     static std::vector<typename SyncFnTy::ValTy> val_vec;
     static std::vector<unsigned int> offsets;

     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     std::string extract_timer_str(syncTypeStr + "_EXTRACT_" + loopName +"_" + get_run_identifier());
     Galois::StatTimer StatTimer_extract(extract_timer_str.c_str());
     StatTimer_extract.start();
     DataCommMode data_mode;

     if (num > 0) {
       bit_set_comm.resize(num);
       offsets.resize(num);
       val_vec.resize(num);
       size_t bit_set_count = 0;

       bool batch_succeeded = extract_batch_wrapper<SyncFnTy, syncType>(from_id, 
                                bit_set_comm, offsets, val_vec, bit_set_count,
                                data_mode);

       // GPUs have a batch function they can use; CPUs do not
       if (!batch_succeeded) {
         const Galois::DynamicBitSet &bit_set_compute = BitsetFnTy::get();

         get_bitset_and_offsets<SyncFnTy, syncType>(loopName, indices, 
                    bit_set_compute, bit_set_comm, offsets, bit_set_count, 
                    data_mode);

         // at this point indices should hold local ids of nodes that need
         // to be accessed
         if (data_mode == onlyData) {
           bit_set_count = indices.size();
           extract_subset<SyncFnTy, syncType, true, true>(loopName, indices, 
             bit_set_count, offsets, val_vec);
         } else if (data_mode != noData) { // bitsetData or offsetsData
           extract_subset<SyncFnTy, syncType, false, true>(loopName, indices, 
             bit_set_count, offsets, val_vec);
         }
       }

       size_t redundant_size = (num - bit_set_count) * 
                                 sizeof(typename SyncFnTy::ValTy);
       size_t bit_set_size = (bit_set_comm.get_vec().size()*sizeof(uint64_t));

       std::string statSavedBytes_str(syncTypeStr + "_SAVED_BYTES_" + loopName +
                                      "_" + get_run_identifier());
       Galois::Statistic SyncReduce_saved_bytes(statSavedBytes_str);
       if (redundant_size > bit_set_size) 
         SyncReduce_saved_bytes += redundant_size - bit_set_size;

       if (data_mode == noData) {
         gSerialize(b, data_mode);
       } else if (data_mode == offsetsData) {
         offsets.resize(bit_set_count);
         if (useGidMetadata) {
           convert_lid_to_gid<syncType>(loopName, offsets);
         }
         val_vec.resize(bit_set_count);
         gSerialize(b, data_mode, bit_set_count, offsets, val_vec);
       } else if (data_mode == bitsetData) {
         val_vec.resize(bit_set_count);
         gSerialize(b, data_mode, bit_set_count, bit_set_comm, val_vec);
       } else { // onlyData
         gSerialize(b, data_mode, val_vec);
       }
     } else {
       data_mode = noData;
       gSerialize(b, noData);
     }

     StatTimer_extract.stop();
     std::string metadata_str(syncTypeStr + "_METADATA_MODE" + std::to_string(data_mode) + "_" + loopName + "_" + get_run_identifier());
     Galois::Statistic Statistic_metadata(metadata_str.c_str());
     Statistic_metadata += 1;
   }

   template<WriteLocation writeLocation, ReadLocation readLocation, 
     SyncType syncType, typename SyncFnTy, typename BitsetFnTy>
   void sync_send(std::string loopName) {
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     Galois::StatTimer StatTimer_SendTime(syncTypeStr + "_SEND_" +  loopName + "_" + get_run_identifier());
     StatTimer_SendTime.start();
     auto &sharedNodes = (syncType == syncReduce) ? mirrorNodes : masterNodes;

     auto& net = Galois::Runtime::getSystemNetworkInterface();
     for (unsigned h = 1; h < net.Num; ++h) {
        unsigned x = (id + h) % net.Num;
        if (nothingToSend(x, syncType, writeLocation, readLocation)) continue;

        Galois::Runtime::SendBuffer b;
        if (BitsetFnTy::is_valid())
          sync_extract<syncType, SyncFnTy, BitsetFnTy>(loopName, x, sharedNodes[x], b);
        else                     
          sync_extract<syncType, SyncFnTy>(loopName, x, sharedNodes[x], b);

        std::string statSendBytes_str(syncTypeStr + "_SEND_BYTES_" + loopName + "_" + get_run_identifier());
        Galois::Statistic SyncReduce_send_bytes(statSendBytes_str);
        SyncReduce_send_bytes += b.size();
        net.sendTagged(x, Galois::Runtime::evilPhase, b);
     }
     // Will force all messages to be processed before continuing
     net.flush();

     if (BitsetFnTy::is_valid()) {
       reset_bitset(syncType, &BitsetFnTy::reset_range);
     }

     StatTimer_SendTime.stop();
   }

   template<SyncType syncType, typename SyncFnTy, typename BitsetFnTy, 
            bool parallelize = true>
   size_t syncRecvApply(uint32_t from_id, Galois::Runtime::RecvBuffer& buf, 
                        std::string loopName) {
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     std::string set_timer_str(syncTypeStr + "_SET_" + loopName + "_" + get_run_identifier());
     Galois::StatTimer StatTimer_set(set_timer_str.c_str());
     StatTimer_set.start();
     static Galois::DynamicBitSet bit_set_comm;
     static std::vector<typename SyncFnTy::ValTy> val_vec;
     static std::vector<unsigned int> offsets;
     auto &sharedNodes = (syncType == syncReduce) ? masterNodes : mirrorNodes;

     uint32_t num = sharedNodes[from_id].size();
     size_t buf_start = 0;
     size_t retval = 0;
     if(num > 0){
       DataCommMode data_mode;
       Galois::Runtime::gDeserialize(buf, data_mode);
       if (data_mode != noData) {
         size_t bit_set_count = num;

         if (data_mode != onlyData) {
           Galois::Runtime::gDeserialize(buf, bit_set_count);
           if (data_mode == offsetsData) {
             //offsets.resize(bit_set_count);
             Galois::Runtime::gDeserialize(buf, offsets);
             if (useGidMetadata) {
               convert_gid_to_lid<syncType>(loopName, offsets);
             }
           } else if (data_mode == bitsetData) {
             bit_set_comm.resize(num);
             Galois::Runtime::gDeserialize(buf, bit_set_comm);
           } else if (data_mode == dataSplit) {
             Galois::Runtime::gDeserialize(buf, buf_start);
           } else if (data_mode == dataSplitFirst) {
             Galois::Runtime::gDeserialize(buf, retval);
           }
         }

         //val_vec.resize(bit_set_count);
         Galois::Runtime::gDeserialize(buf, val_vec);

         bool batch_succeeded = set_batch_wrapper<SyncFnTy, syncType>(from_id, bit_set_comm, offsets, val_vec, bit_set_count, data_mode);
         if (!batch_succeeded) {
           Galois::DynamicBitSet &bit_set_compute = BitsetFnTy::get();
           if (data_mode == bitsetData) {
             size_t bit_set_count2;
             get_offsets_from_bitset<syncType>(loopName, bit_set_comm, offsets, bit_set_count2);
             assert(bit_set_count ==  bit_set_count2);
           }
           if (data_mode == onlyData) {
             set_subset<SyncFnTy, syncType, true, true>(loopName, sharedNodes[from_id], bit_set_count, offsets, val_vec, bit_set_compute);
           } else if (data_mode == dataSplit || data_mode == dataSplitFirst) {
             set_subset<SyncFnTy, syncType, true, true>(loopName, sharedNodes[from_id], bit_set_count, offsets, val_vec, bit_set_compute, buf_start);
           } else {
             set_subset<SyncFnTy, syncType, false, true>(loopName, sharedNodes[from_id], bit_set_count, offsets, val_vec, bit_set_compute);
           }
           // TODO: reduce could update the bitset, so it needs to be copied back to the device
         }
       }
     }
     StatTimer_set.stop();
     return retval;
   }

   template<WriteLocation writeLocation, ReadLocation readLocation, 
     SyncType syncType, typename SyncFnTy, typename BitsetFnTy>
   void sync_recv(std::string loopName, unsigned int num_messages = 1) {
     auto& net = Galois::Runtime::getSystemNetworkInterface();
     std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
     Galois::StatTimer StatTimer_RecvTime(syncTypeStr + "_RECV_" +  loopName + "_" + get_run_identifier());
     StatTimer_RecvTime.start();
     for (unsigned num = 0; num < num_messages; ++num) {
       for (unsigned x = 0; x < net.Num; ++x) {
         if (x == id) continue;
         if (nothingToRecv(x, syncType, writeLocation, readLocation)) continue;
         decltype(net.recieveTagged(Galois::Runtime::evilPhase,nullptr)) p;
         do {
           net.handleReceives();
           p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
         } while (!p);
         syncRecvApply<syncType, SyncFnTy, BitsetFnTy>(p->first, p->second, loopName);
       }
     }
     ++Galois::Runtime::evilPhase;
     StatTimer_RecvTime.stop();
   }

#ifdef __GALOIS_EXP_COMMUNICATION_ALGORITHM__
   template<typename FnTy, SyncType syncType>
   void sync_sendrecv_exp(std::string loopName, 
                          Galois::DynamicBitSet& bit_set_compute, 
                          size_t block_size) {
      std::atomic<unsigned> total_incoming(0);
      std::atomic<unsigned> recved_firsts(0);
      std::atomic<unsigned> sendbytes_count(0);
      std::atomic<unsigned> msg_received(0);

      std::string syncTypeStr = (syncType == syncReduce) ? "REDUCE" : "BROADCAST";
      std::string statSendBytes_str(syncTypeStr + "_SEND_BYTES_" + loopName + "_" + get_run_identifier());
      std::string send_timer_str(syncTypeStr + "_SEND_" + loopName + "_" + get_run_identifier());
      std::string tryrecv_timer_str(syncTypeStr + "_TRYRECV_" + loopName + "_" + get_run_identifier());
      std::string recv_timer_str(syncTypeStr + "_RECV_" + loopName + "_" + get_run_identifier());
      std::string loop_timer_str(syncTypeStr + "_EXP_MAIN_LOOP" + loopName + "_" + get_run_identifier());
      std::string doall_str("LAMBDA::SENDRECV" + loopName + "_" + get_run_identifier());

      Galois::Statistic send_bytes(statSendBytes_str);

      Galois::StatTimer StatTimer_send(send_timer_str.c_str());
      Galois::StatTimer StatTimer_tryrecv(tryrecv_timer_str.c_str());
      Galois::StatTimer StatTimer_RecvTime(recv_timer_str.c_str());
      Galois::StatTimer StatTimer_mainLoop(loop_timer_str.c_str());

      StatTimer_mainLoop.start();

      auto& sharedNodes = (syncType == syncReduce) ? mirrorNodes : masterNodes;
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      std::mutex m;

      for (unsigned h = 1; h < net.Num; ++h) {
        unsigned x = (id + h) % net.Num;
        auto& indices = sharedNodes[x];
        size_t num_elem = indices.size();
        size_t nblocks = (num_elem + (block_size - 1)) / block_size;

        Galois::do_all(boost::counting_iterator<size_t>(0), boost::counting_iterator<size_t>(nblocks), [&](size_t n){
          // ========== Send ==========
          Galois::Runtime::SendBuffer b;
          size_t num = ((n+1) * (block_size) > num_elem) ? (num_elem - (n*block_size)) : block_size;
          size_t start = n * block_size;
          std::vector<unsigned int> offsets;

          if (num > 0){
            if (n == 0) {
              gSerialize(b, dataSplitFirst, num, nblocks);
              auto lseq = gSerializeLazySeq(b, num, (std::vector<typename FnTy::ValTy>*)nullptr);
              extract_subset<FnTy, syncType, decltype(lseq), true, false>(loopName, indices, num, offsets, b, lseq, start);
            } else {
              gSerialize(b, dataSplit, num, start);
              auto lseq = gSerializeLazySeq(b, num, (std::vector<typename FnTy::ValTy>*)nullptr);
              extract_subset<FnTy, syncType, decltype(lseq), true, false>(loopName, indices, num, offsets, b, lseq, start);
            }
          } else {
            // TODO: Send dataSplitFirst with # msg = 1. Will need extra check in syncRecvApply
            gSerialize(b, noData);
          }

          StatTimer_send.start();

          net.sendTagged(x, Galois::Runtime::evilPhase, b);
          net.flush();
          StatTimer_send.stop();

          sendbytes_count += b.size();

          // ========== Try Receive ==========
          decltype(net.recieveTagged(Galois::Runtime::evilPhase,nullptr)) p;

          if (m.try_lock()) {
            net.handleReceives();
            p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
            m.unlock();
          }

          if (p) {
            auto val = syncRecvApply<FnTy, syncType, false>(p->first, p->second, bit_set_compute, loopName);
            if (val != 0) {
              recved_firsts += 1;
              total_incoming += val;
            }
            msg_received += 1;
          }
        }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
      }

      send_bytes += sendbytes_count.load();
      StatTimer_mainLoop.stop();

      // ========== Receive ==========
      StatTimer_RecvTime.start();

      decltype(net.recieveTagged(Galois::Runtime::evilPhase,nullptr)) p;

      while (recved_firsts.load() != (net.Num - 1) || msg_received.load() != total_incoming.load()) {
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
        if (p) {
          auto val = syncRecvApply<FnTy, syncType, true>(p->first, p->second, bit_set_compute, loopName);
          if (val != 0) {
            recved_firsts += 1;
            total_incoming += val;
          }
          msg_received += 1;
        }
      }
      ++Galois::Runtime::evilPhase;

      StatTimer_RecvTime.stop();
   }
#endif

   // reduce from mirrors to master
   template<WriteLocation writeLocation, ReadLocation readLocation, 
     typename ReduceFnTy, typename BitsetFnTy>
   void reduce(std::string loopName) {
#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      if (comm_mode == 1) {
        simulate_reduce<ReduceFnTy>(loopName);
        return;
      } else if (comm_mode == 2) {
        simulate_bare_mpi_reduce<ReduceFnTy>(loopName);
        return;
      }
#endif
#endif
 
      std::string timer_str("REDUCE_" + loopName + "_" + get_run_identifier());
      Galois::StatTimer StatTimer_syncReduce(timer_str.c_str());
      StatTimer_syncReduce.start();

#ifdef __GALOIS_EXP_COMMUNICATION_ALGORITHM__
      size_t block_size = buffSize;
      sync_sendrecv_exp<ReduceFnTy, syncReduce>(loopName, bit_set_compute, block_size);
#else
      sync_send<writeLocation, readLocation, syncReduce, ReduceFnTy, BitsetFnTy>(loopName);
                                                         
      sync_recv<writeLocation, readLocation, syncReduce, ReduceFnTy, BitsetFnTy>(loopName);
#endif

      StatTimer_syncReduce.stop();
   }

   // broadcast from master to mirrors
   template<WriteLocation writeLocation, ReadLocation readLocation, 
     typename BroadcastFnTy, typename BitsetFnTy>
   void broadcast(std::string loopName) {
#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      if (comm_mode == 1) {
        simulate_broadcast<BroadcastFnTy>(loopName);
        return;
      } else if (comm_mode == 2) {
        simulate_bare_mpi_broadcast<BroadcastFnTy>(loopName);
        return;
      }
#endif
#endif
 
      std::string timer_str("BROADCAST_" + loopName + "_" + get_run_identifier());
      Galois::StatTimer StatTimer_syncBroadcast(timer_str.c_str());
      StatTimer_syncBroadcast.start();

#ifdef __GALOIS_EXP_COMMUNICATION_ALGORITHM__
      size_t block_size = buffSize;
      sync_sendrecv_exp<BroadcastFnTy, syncBroadcast>(loopName, block_size);
#else
      sync_send<writeLocation, readLocation, syncBroadcast, BroadcastFnTy, BitsetFnTy>(loopName);
                                                            
      sync_recv<writeLocation, readLocation, syncBroadcast, BroadcastFnTy, BitsetFnTy>(loopName);
#endif

      StatTimer_syncBroadcast.stop();
   }

   // OEC - outgoing edge-cut : source of any edge is master
   // IEC - incoming edge-cut : destination of any edge is master
   // CVC - cartesian vertex-cut : if source of an edge is mirror, then destination is not, and vice-versa
   // UVC - unconstrained vertex-cut
   
   // reduce - mirrors to master
   // broadcast - master to mirrors

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_src_to_src(std::string loopName) {
     // do nothing for OEC
     // reduce and broadcast for IEC, CVC, UVC
     if (transposed || is_vertex_cut()) {
       reduce<writeSource, readSource, ReduceFnTy, BitsetFnTy>(loopName);
       broadcast<writeSource, readSource, BroadcastFnTy, BitsetFnTy>(loopName);
     }
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_src_to_dst(std::string loopName) {
     // only broadcast for OEC
     // only reduce for IEC
     // reduce and broadcast for CVC, UVC
     if (transposed) {
       reduce<writeSource, readDestination, ReduceFnTy, BitsetFnTy>(loopName);
       if (is_vertex_cut()) {
         broadcast<writeSource, readDestination, BroadcastFnTy, BitsetFnTy>(loopName);
       }
     } else {
       if (is_vertex_cut()) {
         reduce<writeSource, readDestination, ReduceFnTy, BitsetFnTy>(loopName);
       }
       broadcast<writeSource, readDestination, BroadcastFnTy, BitsetFnTy>(loopName);
     }
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_src_to_any(std::string loopName) {
     // only broadcast for OEC
     // reduce and broadcast for IEC, CVC, UVC
     if (transposed || is_vertex_cut()) {
       reduce<writeSource, readAny, ReduceFnTy, BitsetFnTy>(loopName);
     }
     broadcast<writeSource, readAny, BroadcastFnTy, BitsetFnTy>(loopName);
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_dst_to_src(std::string loopName) {
     // only reduce for OEC
     // only broadcast for IEC
     // reduce and broadcast for CVC, UVC
     if (transposed) {
       if (is_vertex_cut()) {
         reduce<writeDestination, readSource, ReduceFnTy, BitsetFnTy>(loopName);
       }
       broadcast<writeDestination, readSource, BroadcastFnTy, BitsetFnTy>(loopName);
     } else {
       reduce<writeDestination, readSource, ReduceFnTy, BitsetFnTy>(loopName);
       if (is_vertex_cut()) {
         broadcast<writeDestination, readSource, BroadcastFnTy, BitsetFnTy>(loopName);
       }
     }
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_dst_to_dst(std::string loopName) {
     // do nothing for IEC
     // reduce and broadcast for OEC, CVC, UVC
     if (!transposed || is_vertex_cut()) {
       reduce<writeDestination, readDestination, ReduceFnTy, BitsetFnTy>(loopName);
       broadcast<writeDestination, readDestination, BroadcastFnTy, BitsetFnTy>(loopName);
     }
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_dst_to_any(std::string loopName) {
     // only broadcast for IEC
     // reduce and broadcast for OEC, CVC, UVC
     if (!transposed || is_vertex_cut()) {
       reduce<writeDestination, readAny, ReduceFnTy, BitsetFnTy>(loopName);
     }
     broadcast<writeDestination, readAny, BroadcastFnTy, BitsetFnTy>(loopName);
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_any_to_src(std::string loopName) {
     // only reduce for OEC
     // reduce and broadcast for IEC, CVC, UVC
     reduce<writeAny, readSource, ReduceFnTy, BitsetFnTy>(loopName);
     if (transposed || is_vertex_cut()) {
       broadcast<writeAny, readSource, BroadcastFnTy, BitsetFnTy>(loopName);
     }
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_any_to_dst(std::string loopName) {
     // only reduce for IEC
     // reduce and broadcast for OEC, CVC, UVC
     reduce<writeAny, readDestination, ReduceFnTy, BitsetFnTy>(loopName);
     if (!transposed || is_vertex_cut()) {
       broadcast<writeAny, readDestination, BroadcastFnTy, BitsetFnTy>(loopName);
     }
   }

   template<typename ReduceFnTy, typename BroadcastFnTy, typename BitsetFnTy>
   void sync_any_to_any(std::string loopName) {
     // reduce and broadcast for OEC, IEC, CVC, UVC
     reduce<writeAny, readAny, ReduceFnTy, BitsetFnTy>(loopName);
     broadcast<writeAny, readAny, BroadcastFnTy, BitsetFnTy>(loopName);
   }

public:

   template<WriteLocation writeLocation, ReadLocation readLocation, 
     typename ReduceFnTy, typename BroadcastFnTy, 
     typename BitsetFnTy = Galois::InvalidBitsetFnTy>
   void sync(std::string loopName) {
     std::string timer_str("SYNC_" + loopName + "_" + get_run_identifier());
     Galois::StatTimer StatTimer_sync(timer_str.c_str());
     StatTimer_sync.start();

     if (writeLocation == writeSource) {
       if (readLocation == readSource) {
         sync_src_to_src<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       } else if (readLocation == readDestination) {
         sync_src_to_dst<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       } else { // readAny
         sync_src_to_any<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       }
     } else if (writeLocation == writeDestination) {
       if (readLocation == readSource) {
         sync_dst_to_src<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       } else if (readLocation == readDestination) {
         sync_dst_to_dst<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       } else { // readAny
         sync_dst_to_any<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       }
     } else { // writeAny
       if (readLocation == readSource) {
         sync_any_to_src<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       } else if (readLocation == readDestination) {
         sync_any_to_dst<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       } else { // readAny
         sync_any_to_any<ReduceFnTy, BroadcastFnTy, BitsetFnTy>(loopName);
       }
     }

     StatTimer_sync.stop();
   }

   // just like any other sync_*, this is expected to be a collective call
   // but it does not synchronize with other hosts
   // nonetheless, it should be called same number of times on all hosts
   template<typename ReduceFnTy, typename BroadcastFnTy>
   void sync_dst_to_src_pipe(std::string loopName, Galois::DynamicBitSet& bit_set_compute) {
     if (transposed) {
       if (is_vertex_cut()) {
         sync_send<ReduceFnTy, syncReduce, writeDestination, readSource>(loopName, bit_set_compute);
       } else {
         sync_send<BroadcastFnTy, syncBroadcast, writeDestination, readSource>(loopName, bit_set_compute);
       }
     } else {
       sync_send<ReduceFnTy, syncReduce, writeDestination, readSource>(loopName, bit_set_compute);
     }
     ++numPipelinedPhases;
   }

   template<typename ReduceFnTy, typename BroadcastFnTy>
   void sync_dst_to_src_wait(std::string loopName, Galois::DynamicBitSet& bit_set_compute) {
     if (transposed) {
       if (is_vertex_cut()) {
         sync_recv<ReduceFnTy, syncReduce, writeDestination, readSource>(loopName, bit_set_compute, numPipelinedPhases);
       } else {
         sync_recv<BroadcastFnTy, syncBroadcast, writeDestination, readSource>(loopName, bit_set_compute, numPipelinedPhases);
       }
     } else {
       sync_recv<ReduceFnTy, syncReduce, writeDestination, readSource>(loopName, bit_set_compute, numPipelinedPhases);
     }
     numPipelinedPhases = 0;
     if (is_vertex_cut()) {
       broadcast<BroadcastFnTy, writeDestination, readSource>(loopName, bit_set_compute);
     }
   }

   // just like any other sync_*, this is expected to be a collective call
   // but it does not synchronize with other hosts
   // nonetheless, it should be called same number of times on all hosts
   template<typename ReduceFnTy, typename BroadcastFnTy>
   void sync_src_to_dst_pipe(std::string loopName, Galois::DynamicBitSet& bit_set_compute) {
     if (transposed) {
       sync_send<ReduceFnTy, syncReduce, writeSource, readDestination>(loopName, bit_set_compute);
     } else {
       if (is_vertex_cut()) {
         sync_send<ReduceFnTy, syncReduce, writeSource, readDestination>(loopName, bit_set_compute);
       } else {
         sync_send<BroadcastFnTy, syncBroadcast, writeSource, readDestination>(loopName, bit_set_compute);
       }
     }
     ++numPipelinedPhases;
   }

   template<typename ReduceFnTy, typename BroadcastFnTy>
   void sync_src_to_dst_wait(std::string loopName, Galois::DynamicBitSet& bit_set_compute) {
     if (transposed) {
       sync_recv<ReduceFnTy, syncReduce, writeSource, readDestination>(loopName, bit_set_compute, numPipelinedPhases);
     } else {
       if (is_vertex_cut()) {
         sync_recv<ReduceFnTy, syncReduce, writeSource, readDestination>(loopName, bit_set_compute, numPipelinedPhases);
       } else {
         sync_recv<BroadcastFnTy, syncBroadcast, writeSource, readDestination>(loopName, bit_set_compute, numPipelinedPhases);
       }
     }
     numPipelinedPhases = 0;
     if (is_vertex_cut()) {
       broadcast<BroadcastFnTy, writeSource, readDestination>(loopName, bit_set_compute);
     }
   }

   template<typename FnTy>
   void reduce_ck(std::string loopName) {
#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
      if (comm_mode == 1) {
        simulate_reduce<FnTy>(loopName);
        return;
      } else if (comm_mode == 2) {
        simulate_bare_mpi_reduce<FnTy>(loopName);
        return;
      }
#endif
#endif
      std::string extract_timer_str("REDUCE_EXTRACT_" + loopName +"_" + get_run_identifier());
      std::string timer_str("REDUCE_" + loopName + "_" + get_run_identifier());
      std::string timer_barrier_str("REDUCE_BARRIER_" + loopName + "_" + get_run_identifier());
      std::string statSendBytes_str("SEND_BYTES_REDUCE_" + loopName + "_" + get_run_identifier());
      std::string doall_str("LAMBDA::REDUCE_" + loopName + "_" + get_run_identifier());
      Galois::Statistic SyncReduce_send_bytes(statSendBytes_str);
      Galois::StatTimer StatTimer_syncReduce(timer_str.c_str());
      Galois::StatTimer StatTimerBarrier_syncReduce(timer_barrier_str.c_str());
      Galois::StatTimer StatTimer_extract(extract_timer_str.c_str());

      std::string statChkPtBytes_str("CHECKPOINT_BYTES_REDUCE_" + loopName +"_" + get_run_identifier());
      Galois::Statistic checkpoint_bytes(statChkPtBytes_str);

      std::string checkpoint_timer_str("TIME_CHECKPOINT_REDUCE_MEM_" + get_run_identifier());
      Galois::StatTimer StatTimer_checkpoint(checkpoint_timer_str.c_str());


      StatTimer_syncReduce.start();
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      for (unsigned h = 1; h < net.Num; ++h) {
         unsigned x = (id + h) % net.Num;
         uint32_t num = mirrorNodes[x].size();

         Galois::Runtime::SendBuffer b;

         StatTimer_extract.start();
           std::vector<typename FnTy::ValTy> val_vec(num);

         if(num > 0 ){
           if (!FnTy::extract_reset_batch(x, val_vec.data())) {
             Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(num), [&](uint32_t n){
                  uint32_t lid = mirrorNodes[x][n];
#ifdef __GALOIS_HET_OPENCL__
                  CLNodeDataWrapper d = clGraph.getDataW(lid);
                  auto val = FnTy::extract(lid, getData(lid, d));
                  FnTy::reset(lid, d);
#else
                  auto val = FnTy::extract(lid, getData(lid));
                  FnTy::reset(lid, getData(lid));
#endif
                  val_vec[n] = val;
                 }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
           }

        }

           gSerialize(b, val_vec);
      /*   }
           else {
           gSerialize(b, loopName);
         }
         */


      SyncReduce_send_bytes += b.size();
      auto send_bytes = b.size();

      StatTimer_checkpoint.start();
         if(x == (net.ID + 1)%net.Num){
           //checkpoint owned nodes.
           std::vector<typename FnTy::ValTy> checkpoint_val_vec(numOwned);
           Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(numOwned), [&](uint32_t n) {

               auto val = FnTy::extract(n, getData(n));
               checkpoint_val_vec[n] = val;
               }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
         gSerialize(b, checkpoint_val_vec);
         checkpoint_bytes += (b.size() - send_bytes);

         }
      StatTimer_checkpoint.stop();

         StatTimer_extract.stop();

         net.sendTagged(x, Galois::Runtime::evilPhase, b);
      }
      //Will force all messages to be processed before continuing
      net.flush();

      //receive
      for (unsigned x = 0; x < net.Num; ++x) {
        if ((x == id))
          continue;
        decltype(net.recieveTagged(Galois::Runtime::evilPhase,nullptr)) p;
        do {
          net.handleReceives();
          p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
        } while (!p);
        syncRecvApply_ck<FnTy>(p->first, p->second, loopName);
      }
      ++Galois::Runtime::evilPhase;

      StatTimer_syncReduce.stop();

   }

 /****************************************
  * Fault Tolerance
  * 1. CheckPointing
  ***************************************/
  template <typename FnTy>
  void checkpoint(std::string loopName) {
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    std::string doall_str("LAMBDA::CHECKPOINT_" + loopName + "_" + get_run_identifier());
    std::string checkpoint_timer_str("TIME_CHECKPOINT_" + get_run_identifier());
    std::string checkpoint_fsync_timer_str("TIME_CHECKPOINT_FSYNC_" + get_run_identifier());
    Galois::StatTimer StatTimer_checkpoint(checkpoint_timer_str.c_str());
    Galois::StatTimer StatTimer_checkpoint_fsync(checkpoint_fsync_timer_str.c_str());
    StatTimer_checkpoint.start();


    std::string statChkPtBytes_str("CHECKPOINT_BYTES_" + loopName +"_" + get_run_identifier());
    Galois::Statistic checkpoint_bytes(statChkPtBytes_str);
    //checkpoint owned nodes.
    std::vector<typename FnTy::ValTy> val_vec(numOwned);
    Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(numOwned), [&](uint32_t n) {

          auto val = FnTy::extract(n, getData(n));
          val_vec[n] = val;
        }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));

#if 0
    //Write val_vec to disk.
      if(id == 0)
      for(auto k = 0; k < 10; ++k){
        std::cout << "BEFORE : val_vec[" << k <<"] :" << val_vec[k] << "\n";
      }
#endif


    checkpoint_bytes += val_vec.size() * sizeof(typename FnTy::ValTy);

    //std::string chkPt_fileName = "/scratch/02982/ggill0/Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
    //std::string chkPt_fileName = "Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
    //std::string chkPt_fileName = "CheckPointFiles_" + std::to_string(net.Num) + "/Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);

#ifdef __TMPFS__
#ifdef __CHECKPOINT_NO_FSYNC__
    std::string chkPt_fileName = "/dev/shm/CheckPointFiles_no_fsync_" + std::to_string(net.Num) + "/Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
    Galois::Runtime::reportStat("(NULL)", "CHECKPOINT_FILE_LOC_", chkPt_fileName, 0);
#else
    std::string chkPt_fileName = "/dev/shm/CheckPointFiles_fsync_" + std::to_string(net.Num) + "/Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
    Galois::Runtime::reportStat("(NULL)", "CHECKPOINT_FILE_LOC_", chkPt_fileName, 0);
#endif
    Galois::Runtime::reportStat("(NULL)", "CHECKPOINT_FILE_LOC_", chkPt_fileName, 0);
#else

#ifdef __CHECKPOINT_NO_FSYNC__
    std::string chkPt_fileName = "CheckPointFiles_no_fsync_" + std::to_string(net.Num) + "/Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
    Galois::Runtime::reportStat("(NULL)", "CHECKPOINT_FILE_LOC_", chkPt_fileName, 0);
#else
    std::string chkPt_fileName = "CheckPointFiles_fsync_" + std::to_string(net.Num) + "/Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
    Galois::Runtime::reportStat("(NULL)", "CHECKPOINT_FILE_LOC_", chkPt_fileName, 0);
#endif
#endif

    //std::ofstream chkPt_file(chkPt_fileName, std::ios::out | std::ofstream::binary | std::ofstream::trunc);
#if __TMPFS__
    int fd = shm_open(chkPt_fileName.c_str(),O_CREAT|O_RDWR|O_TRUNC, 0666);
#else
    int fd = open(chkPt_fileName.c_str(),O_CREAT|O_RDWR|O_TRUNC, 0666);
#endif
    if(fd==-1){
      std::cerr << "file could not be created. file name : " << chkPt_fileName << " fd : " << fd << "\n";
      abort();
    }
    write(fd,reinterpret_cast<char*>(val_vec.data()), val_vec.size()*sizeof(typename FnTy::ValTy));
    //chkPt_file.write(reinterpret_cast<char*>(val_vec.data()), val_vec.size()*sizeof(uint32_t));
    StatTimer_checkpoint_fsync.start();
#ifdef __CHECKPOINT_NO_FSYNC__
#else
    fsync(fd);
#endif
    StatTimer_checkpoint_fsync.stop();

    close(fd);
    //chkPt_file.close();
    StatTimer_checkpoint.stop();
  }

  template<typename FnTy>
    void checkpoint_apply(std::string loopName){
      auto& net = Galois::Runtime::getSystemNetworkInterface();
      std::string doall_str("LAMBDA::CHECKPOINT_APPLY_" + loopName + "_" + get_run_identifier());
      //checkpoint owned nodes.
      std::vector<typename FnTy::ValTy> val_vec(numOwned);
      //read val_vec from disk.
      //std::string chkPt_fileName = "/scratch/02982/ggill0/Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
      std::string chkPt_fileName = "Checkpoint_" + loopName + "_" + FnTy::field_name() + "_" + std::to_string(net.ID);
      std::ifstream chkPt_file(chkPt_fileName, std::ios::in | std::ofstream::binary);
      if(!chkPt_file.is_open()){
        std::cout << "Unable to open checkpoint file " << chkPt_fileName << " ! Exiting!\n";
        exit(1);
      }
      chkPt_file.read(reinterpret_cast<char*>(val_vec.data()), numOwned*sizeof(uint32_t));

      if(id == 0)
      for(auto k = 0; k < 10; ++k){
        std::cout << "AFTER : val_vec[" << k << "] : " << val_vec[k] << "\n";
      }

      Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(numOwned), [&](uint32_t n) {

          FnTy::setVal(n, getData(n), val_vec[n]);
          }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
    }

 /*************************************************
  * Fault Tolerance
  * 1. CheckPointing in the memory of another node
  ************************************************/
  template<typename FnTy>
  void saveCheckPoint(Galois::Runtime::RecvBuffer& b){
    checkpoint_recvBuffer = std::move(b);
  }

  template<typename FnTy>
  void checkpoint_mem(std::string loopName) {
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    std::string doall_str("LAMBDA::CHECKPOINT_MEM_" + loopName + "_" + get_run_identifier());

    std::string statChkPtBytes_str("CHECKPOINT_BYTES_" + loopName +"_" + get_run_identifier());
    Galois::Statistic checkpoint_bytes(statChkPtBytes_str);

    std::string checkpoint_timer_str("TIME_CHECKPOINT_TOTAL_MEM_" + get_run_identifier());
    Galois::StatTimer StatTimer_checkpoint(checkpoint_timer_str.c_str());

    std::string checkpoint_timer_send_str("TIME_CHECKPOINT_TOTAL_MEM_SEND_" + get_run_identifier());
    Galois::StatTimer StatTimer_checkpoint_send(checkpoint_timer_send_str.c_str());

    std::string checkpoint_timer_recv_str("TIME_CHECKPOINT_TOTAL_MEM_recv_" + get_run_identifier());
    Galois::StatTimer StatTimer_checkpoint_recv(checkpoint_timer_recv_str.c_str());

    StatTimer_checkpoint.start();

    StatTimer_checkpoint_send.start();
    //checkpoint owned nodes.
    std::vector<typename FnTy::ValTy> val_vec(numOwned);
    Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(numOwned), [&](uint32_t n) {

          auto val = FnTy::extract(n, getData(n));
          val_vec[n] = val;
        }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));

    Galois::Runtime::SendBuffer b;
    gSerialize(b, val_vec);

#if 0
    if(net.ID == 0 )
      for(auto k = 0; k < 10; ++k){
        std::cout << "before : val_vec[" << k << "] : " << val_vec[k] << "\n";
      }
#endif

    checkpoint_bytes += b.size();
    //send to your neighbor on your left.
    net.sendTagged((net.ID + 1)%net.Num, Galois::Runtime::evilPhase, b);

    StatTimer_checkpoint_send.stop();

    net.flush();

    StatTimer_checkpoint_recv.start();
    //receiving the checkpointed data.
    decltype(net.recieveTagged(Galois::Runtime::evilPhase,nullptr)) p;
    do {
      net.handleReceives();
      p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
    } while (!p);
    checkpoint_recvBuffer = std::move(p->second);

    std::cerr << net.ID << " recvBuffer SIZE ::::: " << checkpoint_recvBuffer.size() << "\n";

    ++Galois::Runtime::evilPhase;
    StatTimer_checkpoint_recv.stop();

    StatTimer_checkpoint.stop();
  }

    template<typename FnTy>
    void checkpoint_mem_apply(Galois::Runtime::RecvBuffer& b){
      auto& net = Galois::Runtime::getSystemNetworkInterface();
      std::string doall_str("LAMBDA::CHECKPOINT_MEM_APPLY_" + get_run_identifier());

      std::string checkpoint_timer_str("TIME_CHECKPOINT_MEM_APPLY" + get_run_identifier());
      Galois::StatTimer StatTimer_checkpoint(checkpoint_timer_str.c_str());
      StatTimer_checkpoint.start();

      uint32_t from_id;
      Galois::Runtime::RecvBuffer recv_checkpoint_buf;
      gDeserialize(b, from_id);
      recv_checkpoint_buf = std::move(b);
      std::cerr << net.ID << " : " << recv_checkpoint_buf.size() << "\n";
      //gDeserialize(b, recv_checkpoint_buf);

      std::vector<typename FnTy::ValTy> val_vec(numOwned);
      gDeserialize(recv_checkpoint_buf, val_vec);

      if(net.ID == 0 )
        for(auto k = 0; k < 10; ++k){
          std::cout << "After : val_vec[" << k << "] : " << val_vec[k] << "\n";
        }
      Galois::do_all(boost::counting_iterator<uint32_t>(0), boost::counting_iterator<uint32_t>(numOwned), [&](uint32_t n) {

          FnTy::setVal(n, getData(n), val_vec[n]);
          }, Galois::loopname(doall_str.c_str()), Galois::numrun(get_run_identifier()));
    }


    template<typename FnTy>
    void recovery_help_landingPad(Galois::Runtime::RecvBuffer& buff){
      void (hGraph::*fn)(Galois::Runtime::RecvBuffer&) = &hGraph::checkpoint_mem_apply<FnTy>;
      auto& net = Galois::Runtime::getSystemNetworkInterface();
      uint32_t from_id;
      std::string help_str;
      gDeserialize(buff, from_id, help_str);

      Galois::Runtime::SendBuffer b;
      gSerialize(b, idForSelf(), fn, net.ID, checkpoint_recvBuffer);
      net.sendMsg(from_id, syncRecv, b);

      //send back the checkpointed nodes for from_id.

    }


    template<typename FnTy>
    void recovery_send_help(std::string loopName){
      void (hGraph::*fn)(Galois::Runtime::RecvBuffer&) = &hGraph::recovery_help_landingPad<FnTy>;
      auto& net = Galois::Runtime::getSystemNetworkInterface();
      Galois::Runtime::SendBuffer b;
      std::string help_str = "recoveryHelp";

      gSerialize(b, idForSelf(), fn, net.ID, help_str);

      //send help message to the host that is keeping checkpoint for you.
      net.sendMsg((net.ID + 1)%net.Num, syncRecv, b);
    }


  /*****************************************************/

 /****************************************
  * Fault Tolerance
  * 1. Zorro
  ***************************************/
#if 0
  void recovery_help_landingPad(Galois::Runtime::RecvBuffer& b){
    uint32_t from_id;
    std::string help_str;
    gDeserialize(b, from_id, help_str);

    //send back the mirrorNode for from_id.

  }

  template<typename FnTy>
  void recovery_send_help(std::string loopName){
    void (hGraph::*fn)(Galois::Runtime::RecvBuffer&) = &hGraph::recovery_help<FnTy>;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    Galois::Runtime::SendBuffer b;
    std::string help_str = "recoveryHelp";

    gSerialize(b, idForSelf(), help_str);

    for(auto i = 0; i < net.Num; ++i){
      net.sendMsg(i, syncRecv, b);
    }
  }
#endif


  /*************************************/


   uint64_t getGID(uint32_t nodeID) const {
      return L2G(nodeID);
   }
   uint32_t getLID(uint64_t nodeID) const {
      return G2L(nodeID);
   }
#if 0
   unsigned getHostID(uint64_t gid) {
     getHostID(gid);
   }
#endif
   uint32_t getNumOwned() const {
      return numOwned;
   }
   uint64_t getGlobalOffset() const {
      return globalOffset;
   }
#ifdef __GALOIS_HET_CUDA__
   template<bool isVoidType, typename std::enable_if<isVoidType>::type* = nullptr>
   void setMarshalEdge(MarshalGraph &m, size_t index, edge_iterator &e) {
      // do nothing
   }
   template<bool isVoidType, typename std::enable_if<!isVoidType>::type* = nullptr>
   void setMarshalEdge(MarshalGraph &m, size_t index, edge_iterator &e) {
      m.edge_data[index] = getEdgeData(e);
   }
   MarshalGraph getMarshalGraph(unsigned host_id) {
      assert(host_id == id);
      MarshalGraph m;

      m.nnodes = size();
      m.nedges = sizeEdges();
      m.nowned = std::distance(begin(), end());
      assert(m.nowned > 0);
      m.id = host_id;
      m.row_start = (index_type *) calloc(m.nnodes + 1, sizeof(index_type));
      m.edge_dst = (index_type *) calloc(m.nedges, sizeof(index_type));

      // initialize node_data with localID-to-globalID mapping
      m.node_data = (index_type *) calloc(m.nnodes, sizeof(node_data_type));
      for (index_type i = 0; i < m.nnodes; ++i) {
        m.node_data[i] = getGID(i);
      }

      if (std::is_void<EdgeTy>::value) {
         m.edge_data = NULL;
      } else {
         if (!std::is_same<EdgeTy, edge_data_type>::value) {
            fprintf(stderr, "WARNING: Edge data type mismatch between CPU and GPU\n");
         }
         m.edge_data = (edge_data_type *) calloc(m.nedges, sizeof(edge_data_type));
      }

      // pinched from Rashid's LC_LinearArray_Graph.h
      size_t edge_counter = 0, node_counter = 0;
      for (auto n = begin(); n != ghost_end() && *n != m.nnodes; n++, node_counter++) {
         m.row_start[node_counter] = edge_counter;
         if (*n < m.nowned) {
            for (auto e = edge_begin(*n); e != edge_end(*n); e++) {
               if (getEdgeDst(e) < m.nnodes) {
                  setMarshalEdge<std::is_void<EdgeTy>::value>(m, edge_counter, e);
                  m.edge_dst[edge_counter++] = getEdgeDst(e);
               }
            }
         }
      }

      m.row_start[node_counter] = edge_counter;
      m.nedges = edge_counter;

      // copy memoization meta-data
      m.num_master_nodes = (unsigned int *) calloc(masterNodes.size(), sizeof(unsigned int));;
      m.master_nodes = (unsigned int **) calloc(masterNodes.size(), sizeof(unsigned int *));;
      for(uint32_t h = 0; h < masterNodes.size(); ++h){
        m.num_master_nodes[h] = masterNodes[h].size();
        if (masterNodes[h].size() > 0) {
          m.master_nodes[h] = (unsigned int *) calloc(masterNodes[h].size(), sizeof(unsigned int));;
          std::copy(masterNodes[h].begin(), masterNodes[h].end(), m.master_nodes[h]);
        } else {
          m.master_nodes[h] = NULL;
        }
      }
      m.num_mirror_nodes = (unsigned int *) calloc(mirrorNodes.size(), sizeof(unsigned int));;
      m.mirror_nodes = (unsigned int **) calloc(mirrorNodes.size(), sizeof(unsigned int *));;
      for(uint32_t h = 0; h < mirrorNodes.size(); ++h){
        m.num_mirror_nodes[h] = mirrorNodes[h].size();
        if (mirrorNodes[h].size() > 0) {
          m.mirror_nodes[h] = (unsigned int *) calloc(mirrorNodes[h].size(), sizeof(unsigned int));;
          std::copy(mirrorNodes[h].begin(), mirrorNodes[h].end(), m.mirror_nodes[h]);
        } else {
          m.mirror_nodes[h] = NULL;
        }
      }

      return m;
   }
#endif

#ifdef __GALOIS_HET_OPENCL__
public:
   typedef Galois::OpenCL::Graphs::CL_LC_Graph<NodeTy, EdgeTy> CLGraphType;
   typedef typename CLGraphType::NodeDataWrapper CLNodeDataWrapper;
   typedef typename CLGraphType::NodeIterator CLNodeIterator;
   CLGraphType clGraph;
#endif

#ifdef __GALOIS_HET_OPENCL__
   const cl_mem & device_ptr() {
      return clGraph.device_ptr();
   }
   CLNodeDataWrapper getDataW(GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
      return clGraph.getDataW(N);
   }
   const CLNodeDataWrapper getDataR(GraphNode N,Galois::MethodFlag mflag = Galois::MethodFlag::READ) {
      return clGraph.getDataR(N);
   }

#endif


  uint64_t get_totalEdges() const {
    return totalEdges;
  }
  void reset_num_iter(uint32_t runNum){
     num_run = runNum;
  }
  uint32_t get_run_num() {
    return num_run;
  }
  void set_num_iter(uint32_t iteration){
    num_iteration = iteration;
  }

  std::string get_run_identifier() {
    return std::string(std::to_string(num_run) + "_" + 
                       std::to_string(num_iteration));
  }

  std::string get_run_identifier(std::string loop_name) {
    return std::string(std::string(loop_name) + "_" + std::to_string(num_run) +
                       "_" + std::to_string(num_iteration));
  }

  /** Report stats to be printed.**/
  void reportStats(){
    statGhostNodes.report();
  }

  /**
   * Returns the thread ranges array that specifies division of nodes among
   * threads
   *
   * @returns An array of unsigned ints which spcifies which thread gets which
   * nodes.
   */
  const uint32_t* get_thread_ranges() {
    return graph.getThreadRanges();
  }

  /**
   * Returns the thread ranges array that specifies division of nodes among
   * threads for only master nodes
   *
   * @returns An array of unsigned ints which spcifies which thread gets which
   * nodes.
   */
  const uint32_t* get_thread_ranges_master() {
    return masterRanges.data();
  }

  /**
   * Returns the thread ranges array that specifies division of nodes among
   * threads for only nodes with edges
   *
   * @returns An array of unsigned ints which spcifies which thread gets which
   * nodes.
   */
  const uint32_t* get_thread_ranges_with_edges() {
    return withEdgeRanges.data();
  }

  //////////////////////////////////////////////////////////////////////////////
  // Calls for the container object for do_all_local + other things that need
  // this container interface
  //////////////////////////////////////////////////////////////////////////////

  /**
   * Returns an iterator to the first node assigned to the thread that calls
   * this function
   *
   * @returns An iterator to the first node that a thread is to do work on.
   */
  local_iterator local_begin() const {
    const uint32_t* thread_ranges = graph.getThreadRanges();

    if (thread_ranges) {
      uint32_t my_thread_id = Galois::Substrate::ThreadPool::getTID();
      return begin() + thread_ranges[my_thread_id];
    } else {
      return Galois::block_range(begin(), end(), 
                                 Galois::Substrate::ThreadPool::getTID(),
                                 Galois::Runtime::activeThreads).first;
    }
  }

  /**
   * Returns an iterator to the node after the last node that the thread
   * that calls this function is assigned to work on.
   *
   * @returns An iterator to the node after the last node the thread is
   * to work on.
   */
  local_iterator local_end() const {
    const uint32_t* thread_ranges = graph.getThreadRanges();

    if (thread_ranges) {
      uint32_t my_thread_id = Galois::Substrate::ThreadPool::getTID();
      local_iterator to_return = begin() + 
                                 thread_ranges[my_thread_id + 1];
       
      // quick safety check to make sure it doesn't go past the end of the
      // graph
      if (to_return > end()) {
        printf("WARNING: local end iterator goes past the end of the graph\n");
      }

      return to_return;
    } else {
      return Galois::block_range(begin(), end(), 
                                 Galois::Substrate::ThreadPool::getTID(),
                                 Galois::Runtime::activeThreads).second;
    }
  }

  //////////////////////////////////////////////////////////////////////////////


  void save_local_graph(std::string folder_name, std::string local_file_name){

    std::string graph_GID_file_name_str = folder_name + "/graph_GID_" + local_file_name + ".edgelist.PART." + std::to_string(id) + ".OF." + std::to_string(numHosts);
    std::string graph_LID_DIMACS_file_name_str = folder_name + "/graph_LID_" + local_file_name + ".dimacs.PART." + std::to_string(id) + ".OF." + std::to_string(numHosts);
    std::cerr << "SAVING LOCAL GRAPH TO FILE : " << graph_GID_file_name_str << "\n";
    std::ofstream graph_GID_edgelist, graph_LID_dimacs;
    graph_GID_edgelist.open(graph_GID_file_name_str.c_str());
    graph_LID_dimacs.open(graph_LID_DIMACS_file_name_str.c_str());

      std::string meta_file_str = folder_name + "/" + local_file_name +".gr.META." + std::to_string(id) + ".OF." + std::to_string(numHosts);
      //std::string tmp_meta_file_str = folder_name + "/" + local_file_name +".gr.TMP." + std::to_string(id) + ".OF." + std::to_string(numHosts);
      std::ofstream meta_file(meta_file_str.c_str());
      //std::ofstream tmp_file;
      //tmp_file.open(tmp_meta_file_str.c_str());

      size_t num_nodes = (size_t)numOwned;
      std::cerr << id << "  NUMNODES  : " <<  num_nodes << "\n";
      meta_file.write(reinterpret_cast<char*>(&num_nodes), sizeof(num_nodes));

      graph_LID_dimacs << "p " << num_nodes << " " << numOwned_edges<<"\n";
      for(size_t lid = 0; lid < numOwned; ++lid){
        //for(auto src = graph.begin(), src_end = graph.end(); src != src_end; ++src){
        size_t src_GID = L2G(lid);
        //size_t lid = (size_t)(*src);
        //size_t gid = (size_t)L2G(*src);
        size_t owner = getOwner_lid(lid);
        for(auto e = graph.edge_begin(lid, Galois::MethodFlag::UNPROTECTED), e_end = graph.edge_end(lid); e != e_end; ++e){
          auto dst = graph.getEdgeDst(e);
          auto edge_wt = graph.getEdgeData(e);
          auto dst_GID = L2G(dst);

          graph_GID_edgelist << src_GID << " " << dst_GID << " " << id <<  "\n";
          graph_LID_dimacs << lid + 1 << " " << dst + 1 << " " <<  edge_wt <<  "\n";
        }
    meta_file.write(reinterpret_cast<char*>(&src_GID), sizeof(src_GID));
    meta_file.write(reinterpret_cast<char*>(&lid), sizeof(lid));
    meta_file.write(reinterpret_cast<char*>(&owner), sizeof(owner));

    //tmp_file << src_GID << " " << lid << " " << owner << "\n";
    }

    //save_meta_file(local_file_name);
    meta_file.close();
    //tmp_file.close();
    graph_GID_edgelist.close();
    graph_LID_dimacs.close();
    }
  };
#endif//_GALOIS_DIST_HGRAPH_H
