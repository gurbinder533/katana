#include <cassert>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>

#include "llvm/Support/CommandLine.h"

using namespace std::chrono;
namespace cll = llvm::cl;

enum GenType { Nodes, FriendConA, FriendConB, AssetConA, AssetConB, BigNodes, NodesToInsert, EdgesToInsert };

static cll::opt<GenType> GenType(
    "GenType", cll::desc("Available generation types:"),
    cll::values(
        clEnumVal(Nodes, "generate nodes file"),
        clEnumVal(NodesToInsert, "generate new node to insert"),
        clEnumVal(EdgesToInsert, "generate new edges to insert"),
        clEnumVal(BigNodes, "generate 2^32 "),
        clEnumVal(FriendConA, "Friend connection A"),
        clEnumVal(FriendConB, "Friend connection B"),
        clEnumVal(AssetConA, "Asset connection A"),
        clEnumVal(AssetConB, "Asset connection B")));

static cll::opt<std::string> dir_name("dir_name", cll::desc("Specify the directory to write output (default value ./)"), cll::init("./"));
static cll::opt<uint64_t> blockID("block_num", cll::desc("Specify the block number to process 0-1999"), cll::value_desc("0-1999"));

static cll::opt<uint64_t> user_num_nodes("num_nodes", cll::desc("Specify the number of nodes to generate (default value 20 billion)"), cll::init(20000000000));
static cll::opt<uint64_t> num_nodes_to_insert("num_nodes_to_insert", cll::desc("Specify the number of new nodes to generate for insertion (default value 80 thousand)"), cll::init(80000));
static cll::opt<uint64_t> num_edges_to_insert("num_edges_to_insert", cll::desc("Specify the number of new edges to generate for insertion (default value 0 thousand)"), cll::init(0));
static cll::opt<uint64_t> start_id_insertion("start_id_insertion", cll::desc("Specify the start id for the nodes to insert (default value 0)"), cll::init(0));

void
CreateNodeDegreeWithMean(
    uint64_t num_nodes, [[maybe_unused]] uint64_t num_edges,
    [[maybe_unused]] std::vector<uint64_t>& node_degree, float prob1,
    uint32_t mean1, float prob2, uint32_t mean2, float prob3, uint32_t mean3,
    float prob4, uint32_t mean4, [[maybe_unused]] uint64_t max_degree) {
  // Number of nodes in each range
  uint64_t num_type1 = prob1 * num_nodes;  //1-99
  uint64_t num_type2 = prob2 * num_nodes;  //100-999
  uint64_t num_type3 = prob3 * num_nodes;  //1000-9999
  uint64_t num_type4 = prob4 * num_nodes;  //>=10000

  assert((prob1 + prob2 + prob3 + prob4) <= 1);

  /** 
     * Fill node_degree for range1
     */
  double lower = 1;
  double upper = (3 * mean1 - lower) + 1;
  double triangle_tip = 1;  //3 * mean1 - (1 + 99);
  std::random_device dev;
  std::mt19937_64 eng;
  std::piecewise_linear_distribution<> dist(
      {lower, triangle_tip, upper},
      [&](double x) { return x == triangle_tip ? 1. : 0.; });
  uint64_t current_num_edges = 0;
  std::cout << "Range: " << 0 << " : " << num_type1 << "\n";
  for (uint64_t n = 0; n < num_type1; ++n) {
    node_degree[n] = uint64_t(dist(eng));
    current_num_edges += node_degree[n];
  }

  //Add 1k nodes high degree nodes in this range
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dist_node_id(
      0, num_type1 - 1);  // distribution in range [0, num_nodes]
  std::mt19937 rng1(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dist_type1(
      50, 99);  // distribution in range [1, 99]

  if (num_type1 > 0) {
    for (uint64_t n = 0; n < 1000; ++n) {
      uint64_t node_id = dist_node_id(rng);
      node_degree[node_id] = uint64_t(dist_type1(rng1));
      current_num_edges += node_degree[node_id];
    }
  }
  std::cout << "current num edges : " << current_num_edges
            << " : MEAN : " << current_num_edges / num_type1 << "\n";

  /** 
     * Fill node_degree for range2
     */
  double lower2 = 100;
  double upper2 = (3 * mean2 - lower2) + 1;
  double triangle_tip2 = 1;  //3 * mean1 - (1 + 99);
  std::mt19937_64 eng2;
  std::piecewise_linear_distribution<> dist2(
      {lower2, triangle_tip2, upper2},
      [&](double x) { return x == triangle_tip2 ? 1. : 0.; });

  std::cout << "Range: " << num_type1 << " : " << num_type1 + num_type2 << "\n";
  for (uint64_t n = num_type1; n < num_type1 + num_type2; ++n) {
    node_degree[n] = uint64_t(dist2(eng2));
    current_num_edges += node_degree[n];
  }

  //Add 1k nodes high degree nodes in this range
  std::uniform_int_distribution<std::mt19937::result_type> dist_node_id2(
      num_type1,
      num_type1 + num_type2 - 1);  // distribution in range [0, num_nodes]
  std::mt19937 rng2(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dist_type2(
      mean2, 999);  // distribution in range [100, 999]

  if (num_type2 > 0) {
    for (uint64_t n = 0; n < 1000; ++n) {
      uint64_t node_id = dist_node_id2(rng);
      node_degree[node_id] = uint64_t(dist_type2(rng2));
      current_num_edges += node_degree[node_id];
    }
  }

  std::cout << "current num edges : " << current_num_edges
            << " : MEAN : " << current_num_edges / (num_type1 + num_type2)
            << "\n";

  /** 
     * Fill node_degree for range3
     */
  double lower3 = 1000;
  double upper3 = (3 * mean3 - lower3) + 1;
  double triangle_tip3 = 1;  //3 * mean1 - (1 + 99);
  std::mt19937_64 eng3;
  std::piecewise_linear_distribution<> dist3(
      {lower3, triangle_tip3, upper3},
      [&](double x) { return x == triangle_tip3 ? 1. : 0.; });

  std::cout << "Range: " << num_type1 + num_type2 << " : "
            << num_type1 + num_type2 + num_type3 << "\n";
  for (uint64_t n = num_type1 + num_type2;
       n < num_type1 + num_type2 + num_type3; ++n) {
    node_degree[n] = uint64_t(dist3(eng3));
    current_num_edges += node_degree[n];
  }

  std::cout << "current num edges : " << current_num_edges << " : MEAN : "
            << current_num_edges / (num_type1 + num_type2 + num_type3) << "\n";

  /** 
     * Fill node_degree for range4
     */
  double lower4 = 10000;
  double upper4 = (3 * mean4 - lower4) + 1;
  double triangle_tip4 = 1;  //3 * mean1 - (1 + 99);
  std::mt19937_64 eng4;
  std::piecewise_linear_distribution<> dist4(
      {lower4, triangle_tip4, upper4},
      [&](double x) { return x == triangle_tip4 ? 1. : 0.; });

  std::cout << "Range: " << num_type1 + num_type2 + num_type3 << " : "
            << num_type1 + num_type2 + num_type3 + num_type4 << "\n";
  for (uint64_t n = num_type1 + num_type2 + num_type3;
       n < num_type1 + num_type2 + num_type3 + num_type4; ++n) {
    node_degree[n] = uint64_t(dist4(eng4));
    current_num_edges += node_degree[n];
  }

  // Randomly pick the highest degree node from range4 or range3 if range4 is 0
  if (num_type4 > 0) {
    std::mt19937 rng4(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist_node_id4(
        (num_type1 + num_type2 + num_type3),
        (num_type1 + num_type2 + num_type3 + num_type4 - 1));
    node_degree[dist_node_id4(rng4)] = max_degree;
    current_num_edges += max_degree;
  } else {
    std::mt19937 rng3(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist_node_id3(
        (num_type1 + num_type2), (num_type1 + num_type2 + num_type3 - 1));
    node_degree[dist_node_id3(rng3)] = max_degree;
    current_num_edges += max_degree;
  }

  std::cout << "FINAL num edges : " << current_num_edges << "\n";
  std::cout << "FINAL : MEAN : " << current_num_edges / (num_nodes) << "\n";
}

///////////////////// Write to files //////////////////////////

std::string
RandomYesNo() {
  if (rand() % 2 == 0) {
    return std::string("Y");
  } else {
    return std::string("N");
  }
}

int
RandomYesNoInt() {
  if (rand() % 2 == 0) {
    return 1;
  } else {
    return 0;
  }
}

std::string
random_string(std::size_t length) {
  const std::string CHARACTERS =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  std::random_device random_device;
  std::mt19937 generator(random_device());
  std::uniform_int_distribution<> distribution(1, CHARACTERS.size() - 1);

  std::string random_string;

  for (std::size_t i = 0; i < length; ++i) {
    random_string += CHARACTERS[distribution(generator)];
  }

  return random_string;
}
std::string
random_string() {
  const std::string CHARACTERS =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  std::random_device random_device;
  std::mt19937 generator(random_device());
  std::uniform_int_distribution<> distribution_length(1, 50);
  std::uniform_int_distribution<> distribution(1, CHARACTERS.size() - 1);

  std::string random_string;
  std::size_t length = distribution_length(generator);
  for (std::size_t i = 0; i < length; ++i) {
    random_string += CHARACTERS[distribution(generator)];
  }

  return random_string;
}

template <class TimePoint>
class uniform_time_distribution {
public:
  uniform_time_distribution(TimePoint start, TimePoint end)
      : m_start(start),
        m_end(end),
        m_seconds(
            std::chrono::duration_cast<std::chrono::seconds>(end - start)) {}

  template <class Generator>
  TimePoint operator()(Generator&& g) {
    std::uniform_int_distribution<std::chrono::seconds::rep> d(
        0, m_seconds.count());

    return m_start + std::chrono::seconds(d(g));
  }

private:
  TimePoint m_start;
  TimePoint m_end;
  std::chrono::seconds m_seconds;
};

template <class TimePoint>
std::tm
randomTime(TimePoint start, TimePoint end) {
  static std::random_device rd;
  static std::mt19937 gen(rd());

  // TimePoint start = (system_clock::now() - hours(44 * 365 * 10));
  // TimePoint end = (system_clock::now());
  uniform_time_distribution<TimePoint> t(start, end);

  // return t(gen);

  auto time = system_clock::to_time_t(t(gen));
  std::tm timestamp = *std::localtime(&time);
  return timestamp;
  // std::put_time(&timestamp, "%FT%T\n");
  // auto time = system_clock::to_time_t(randomTime(
  //   system_clock::now() - hours(44 * 365 * 10),
  //   system_clock::now()));

  // std::tm timestamp = *std::localtime(&time);
  // std::cout << std::put_time(&timestamp, "%FT%T\n");
}

std::string
returnYesWithBias(uint32_t value) {
  std::random_device dev;
  std::mt19937_64 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> number(
      0, 100);  // distribution in range [0, num_nodes]

  if (uint64_t(number(rng)) < value) {
    return "Y";
  } else {
    return "N";
  }
}

void
CreateNodeFile(
    std::string file_name, [[maybe_unused]] uint64_t num_nodes, float prob, uint64_t start_ID, bool is_inserting=false) {
  std::ofstream node_file(file_name);
  if(is_inserting) {
	  node_file << "creationTime|";
  }	  
  node_file << "id|userIsSuspended|userIsBlacklisted|suspended|strictSuspended|"
               "firstName|";
  node_file << "lastName|phoneIsVerified|daysOnFile|isActive|"
               "accountStatusCode|isClosedPit|is_blacklisted|";
  node_file << "blacklistedDate|sumSent|cntSent|counterparty|cntAppr|cntDecl|"
               "cntRiskDecl1|amtRiskDecl|cntRiskDecl2|sumRcvd|cntRcvd|cntDist|"
               "cntDecl2\n";

  std::random_device random_device;
  std::mt19937 generator(random_device());

  std::uniform_int_distribution<> distribution(0, 1000); 

  std::uniform_int_distribution<> distribution_big_int(
      0, std::numeric_limits<int>::max());

  std::mt19937 generator_double(random_device());
  std::uniform_real_distribution<> distribution_double(0, 10000000000);

  std::uniform_real_distribution<> distribution_double_epoch(
      10000000000, 2606377600000);

  std::random_device dev;
  std::mt19937 rng(dev());

//  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
//      0, num_nodes - 1);

  uint64_t total_nodes = start_ID + num_nodes; 
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
      start_ID, total_nodes - 1);

  // pick percentage of suspended and blacklisted
  uint64_t num_prob = prob * num_nodes;
  std::cout << "Suspended : " << num_prob << "\n";
  std::cout << "prob : " << prob << "\n";
  std::cout << "num nodes : " << num_nodes << "\n";
  std::vector<uint64_t> suspended_vec(num_prob, 0);
  std::vector<uint64_t> strict_suspended_vec(num_prob, 0);
  std::vector<uint64_t> blacklisted_vec(num_prob, 0);
  std::set<uint64_t> numbers1;
  std::set<uint64_t> numbers2;
  std::set<uint64_t> numbers3;
  while (numbers1.size() < num_prob) {
    numbers1.insert(uint64_t(dest_node_id(rng)));
  }
  while (numbers2.size() < num_prob) {
    numbers2.insert(uint64_t(dest_node_id(rng)));
  }
  while (numbers3.size() < num_prob) {
    numbers3.insert(uint64_t(dest_node_id(rng)));
  }

  std::cout << "DONE\n";

  auto it_set1 = numbers1.begin();
  auto it_set2 = numbers2.begin();
  auto it_set3 = numbers3.begin();
  for (uint64_t n = 0; n < num_prob; ++n) {
    suspended_vec[n] = *it_set1;  //uint64_t(dest_node_id(rng));
    ++it_set1;
    strict_suspended_vec[n] = *it_set2;  //uint64_t(dest_node_id(rng));
    ++it_set2;
    blacklisted_vec[n] = *it_set3;  //uint64_t(dest_node_id(rng));
    ++it_set3;
  }
  std::sort(suspended_vec.begin(), suspended_vec.end());
  std::sort(strict_suspended_vec.begin(), strict_suspended_vec.end());
  std::sort(blacklisted_vec.begin(), blacklisted_vec.end());

  auto it = std::unique(suspended_vec.begin(), suspended_vec.end());
  bool wasUnique = (it == suspended_vec.end());
  std::cout << "00000>" << wasUnique << "\n";

  uint64_t curr_index_suspended = 0;
  uint64_t curr_index_strict_suspended = 0;
  uint64_t curr_index_blacklisted = 0;

// tcook2
//  for (uint64_t n = 0; n < num_nodes; ++n) {
  for (uint64_t n = start_ID; n < start_ID+num_nodes; ++n) {
    std::string suspended_string = "N";
    std::string strict_suspended_string = "N";
    std::string blacklisted_string = "N";

    if ((curr_index_suspended < num_prob) &&
        suspended_vec[curr_index_suspended] == n) {
      suspended_string = "Y";
      curr_index_suspended++;
      //std::cout << "S : " << curr_index_suspended << "\n";
    }

    if ((curr_index_strict_suspended < num_prob) &&
        strict_suspended_vec[curr_index_strict_suspended] == n) {
      strict_suspended_string = "Y";
      curr_index_strict_suspended++;
    }
    if ((curr_index_blacklisted < num_prob) &&
        blacklisted_vec[curr_index_blacklisted] == n) {
      blacklisted_string = "Y";
      curr_index_blacklisted++;
      //std::cout << "B : " << n << "\n";
    }

    std::string phone_verified_string =
        returnYesWithBias(98);  //~98% phones verified

    std::string account_status = "active";
    if (suspended_string == "Y" || strict_suspended_string == "Y") {
      account_status = "suspended";
    } else if (blacklisted_string == "Y") {
      account_status = "blacklisted";
    } else {
      if (RandomYesNoInt() == 0) {
        account_status = "inactive";
      }
    }

    uint32_t is_active = (account_status == "active") ? 1 : 0;

  if(is_inserting) {
    auto creation_time = randomTime(
        system_clock::now(), (system_clock::now()));
    node_file << std::put_time(&creation_time, "%FT%T") << "|";  //creationTime
  }
    node_file << n << "|";
    node_file << suspended_string << "|";         //useIsSuspended
    node_file << blacklisted_string << "|";       //userIsBlacklisted
    node_file << suspended_string << "|";         //suspended
    node_file << strict_suspended_string << "|";  //strictSuspended

    node_file << random_string() << "|";          //firstName
    node_file << random_string() << "|";          //lastName
    node_file << phone_verified_string << "|";    //phoneIsVerified
    node_file << distribution(generator) << "|";  //daysOnFile
    node_file << is_active << "|";                //isActive
    node_file << account_status << "|";           //accountStatusCode
    node_file << uint64_t(distribution_double_epoch(generator))
              << "|";                                           //isClosedPit
    node_file << ((blacklisted_string == "Y") ? 1 : 0) << "|";  //is_blacklisted

    auto tm = randomTime(
        (system_clock::now() - hours(44 * 365 * 10)), (system_clock::now()));

    node_file << std::put_time(&tm, "%FT%T") << "|";  //blacklistedDate
    node_file << distribution_double(generator_double) << "|";  //sumSent
    node_file << distribution_big_int(generator) << "|";        //cntSent
    node_file << distribution_big_int(generator) << "|";        //counterparty
    node_file << std::setprecision(4) << std::fixed
              << distribution_double(generator_double) << "|";  //cntAppr
    node_file << std::setprecision(4) << std::fixed
              << distribution_double(generator_double) << "|";  //cntDecl
    node_file << std::setprecision(4) << std::fixed
              << distribution_double(generator_double) << "|";  //cntRiskDecl1
    node_file << std::setprecision(4) << std::fixed
              << distribution_double(generator_double) << "|";  //amtRiskDecl
    node_file << distribution_big_int(generator) << "|";        //cntRiskDecl2
    node_file << distribution_double(generator_double) << "|";  //sumRcvd
    node_file << distribution_big_int(generator) << "|";        //cntRcvd
    node_file << distribution_big_int(generator) << "|";        //cntDist
    node_file << std::setprecision(4) << std::fixed
              << distribution_double(generator_double) << "|\n";  //cntDecl2
  }

  std::cout << "Suspended : " << curr_index_suspended << "\n";
  std::cout << "strict Suspended : " << curr_index_strict_suspended << "\n";
  std::cout << "blacklisted : " << curr_index_blacklisted << "\n";
}

void
CreateEdgesAsset(
    std::string file_name, const std::vector<uint64_t>& node_degree,
    std::string edge_type) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
      0, node_degree.size() - 1);  // distribution in range [0, num_nodes]

  uint64_t count = 0;
  uint64_t batch_size = 10000000;
  uint64_t end = 0;
  uint32_t part = 0;
  for (; count < node_degree.size(); count += batch_size) {
    std::string file_name_part = file_name + "_" + std::to_string(part);
    std::ofstream edge_file(file_name_part);
    edge_file << "Account1.id|Account2.id|type|date\n";
    std::cout << "Count : " << count << "\n";
    if (count + batch_size <= node_degree.size()) {
      end = count + batch_size;
    } else {
      end = node_degree.size();
    }
    for (uint32_t n = count; n < end; ++n) {
      uint64_t degree = node_degree[n];
      while (degree) {
        uint32_t dest_id = uint32_t(dest_node_id(rng));
        auto tm = randomTime(
            (system_clock::now() - hours(44 * 365 * 10)),
            (system_clock::now()));

        edge_file << n << "|" << dest_id << "|" << edge_type << "|"
                  << std::put_time(&tm, "%FT%T") << "\n";
        --degree;
      }
    }
    edge_file.close();
    part++;
  }
}

void
CreateEdgesAssetSplitByEdges(
    std::string file_name, const std::vector<uint64_t>& node_degree,
    std::string edge_type) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
      0, node_degree.size() - 1);  // distribution in range [0, num_nodes]

  uint64_t count = 0;
  uint64_t batch_size = 100000000;
  uint32_t part = 0;
  uint64_t n = 0;

  std::string file_name_part = file_name + "_" + std::to_string(part);
  std::ofstream edge_file(file_name_part);
  edge_file << "Account1.id|Account2.id|type|date\n";
  std::cout << "Count : " << count << "\n";

  for (; n < node_degree.size(); ++n) {
    uint64_t degree = node_degree[n];
    while (degree) {
      count++;
      uint64_t dest_id = uint64_t(dest_node_id(rng));
      auto tm = randomTime(
          (system_clock::now() - hours(44 * 365 * 10)), (system_clock::now()));

      edge_file << n << "|" << dest_id << "|" << edge_type << "|"
                << std::put_time(&tm, "%FT%T") << "\n";
      --degree;

      if (count >= batch_size) {
        std::cout << "Count : " << count << "\n";
        edge_file.close();
        part++;
        count = 0;
        std::string file_name_part = file_name + "_" + std::to_string(part);
        edge_file.open(file_name_part);
        edge_file << "Account1.id|Account2.id|type|date\n";
      }
    }
  }
}

void
CreateEdgesFriend(
    std::string file_name, std::vector<uint64_t>& node_degree,
    std::string edge_type) {
  uint32_t num_nodes = node_degree.size();
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
      0, num_nodes - 1);  // distribution in range [0, num_nodes]

  uint64_t count = 0;
  uint64_t batch_size = 100000000;
  uint64_t end = 0;
  uint32_t part = 0;
  for (; count < node_degree.size(); count += batch_size) {
    std::string file_name_part = file_name + "_" + std::to_string(part);
    std::ofstream edge_file(file_name_part);

    edge_file << "Account1.id|Account2.id|type|date\n";
    std::cout << "Count : " << count << "\n";
    if (count + batch_size <= node_degree.size()) {
      end = count + batch_size;
    } else {
      end = node_degree.size();
    }
    for (uint32_t n = count; n < end; ++n) {
      // uint64_t degree = node_degree[n];
      while (node_degree[n] > 0) {
        uint32_t dest_id = uint32_t(dest_node_id(rng));
        while (dest_id == n) {
          dest_id = uint32_t(dest_node_id(rng));
        }
        if (node_degree[dest_id] > 0) {
          auto tm = randomTime(
              (system_clock::now() - hours(44 * 365 * 10)),
              (system_clock::now()));
          edge_file << n << "|" << dest_id << "|" << edge_type << "|"
                    << std::put_time(&tm, "%FT%T") << "\n";
          edge_file << dest_id << "|" << n << "|" << edge_type << "|"
                    << std::put_time(&tm, "%FT%T") << "\n";
          node_degree[n]--;
          node_degree[dest_id]--;
        } else {
          uint32_t next_dest = dest_id;
          while (node_degree[next_dest] == 0) {
            if (next_dest == num_nodes - 1) {
              next_dest = 0;
            } else {
              next_dest++;
            }
          }
          auto tm = randomTime(
              (system_clock::now() - hours(44 * 365 * 10)),
              (system_clock::now()));

          edge_file << n << "|" << next_dest << "|" << edge_type << "|"
                    << std::put_time(&tm, "%FT%T") << "\n";
          edge_file << next_dest << "|" << n << "|" << edge_type << "|"
                    << std::put_time(&tm, "%FT%T") << "\n";
          node_degree[n]--;
          node_degree[next_dest]--;
        }
      }
    }
    edge_file.close();
    part++;
  }
}

void
CreateEdgesFriendSplitByEdges(
    std::string file_name, std::vector<uint64_t>& node_degree,
    [[maybe_unused]] std::string edge_type) {
  uint64_t num_nodes = node_degree.size();
  std::random_device dev;
  std::mt19937_64 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
      0, num_nodes - 1);  // distribution in range [0, num_nodes]

  uint64_t count = 0;
  uint64_t batch_size = 100000000;
  uint32_t part = 0;
  uint64_t n = 0;

  std::string file_name_part = file_name + "_" + std::to_string(part);
  std::ofstream edge_file(file_name_part);
  edge_file << "Account1.id|Account2.id|type|date\n";
  std::cout << "Count : " << count << "\n";
  // uint32_t prev_dest = 0;
  std::vector<uint64_t> prev_dest_vec;
  uint32_t backups = 10000;
  prev_dest_vec.reserve(backups);
  for (; n < node_degree.size(); ++n) {
    //std::cerr << "n : " << n << " : " << node_degree[n] << "\n";
    while (node_degree[n] > 0) {
      uint64_t dest_id = uint64_t(dest_node_id(rng));
      while (dest_id == n) {
        dest_id = uint64_t(dest_node_id(rng));
      }
      if (node_degree[dest_id] > 0) {
        auto tm = randomTime(
            (system_clock::now() - hours(44 * 365 * 10)),
            (system_clock::now()));
        edge_file << n << "|" << dest_id << "|" << edge_type << "|"
                  << std::put_time(&tm, "%FT%T") << "\n";
        edge_file << dest_id << "|" << n << "|" << edge_type << "|"
                  << std::put_time(&tm, "%FT%T") << "\n";
        count += 2;
        node_degree[n]--;
        node_degree[dest_id]--;
        if (prev_dest_vec.size() < backups) {
          prev_dest_vec.push_back(dest_id);
        }
      } else {
        uint64_t next_dest = uint64_t(dest_node_id(rng));
        while (next_dest == n) {
          next_dest = uint64_t(dest_node_id(rng));
        }
        if (node_degree[next_dest] == 0) {
          uint32_t i = 0;
          for (; i < prev_dest_vec.size(); ++i) {
            if (next_dest != n && node_degree[prev_dest_vec[i]] > 0) {
              break;
            }
          }
          next_dest = prev_dest_vec[i];
        }
        uint64_t num_nodes_encountered = 0;
        if (node_degree[next_dest] == 0) {
          prev_dest_vec.clear();
          prev_dest_vec.resize(0);
          prev_dest_vec.reserve(backups);
        }
        while (next_dest == n || node_degree[next_dest] == 0) {
          num_nodes_encountered++;
          if (next_dest == num_nodes - 1) {
            next_dest = 0;
          } else {
            next_dest++;
          }
          if (num_nodes_encountered == num_nodes) {
            std::cerr << "Something is wrong\n";
            std::cerr << "Nodes encountered : " << num_nodes_encountered
                      << "\n";
            break;
          }
        }
        if (next_dest != n && node_degree[next_dest] > 0) {
          auto tm = randomTime(
              (system_clock::now() - hours(44 * 365 * 10)),
              (system_clock::now()));

          edge_file << n << "|" << next_dest << "|" << edge_type << "|"
                    << std::put_time(&tm, "%FT%T") << "\n";
          edge_file << next_dest << "|" << n << "|" << edge_type << "|"
                    << std::put_time(&tm, "%FT%T") << "\n";
          node_degree[n]--;
          node_degree[next_dest]--;
          count += 2;
          if (prev_dest_vec.size() < backups) {
            prev_dest_vec.push_back(next_dest);
          }
        } else {
          std::cerr << "Cound not find the destination\n";
          std::cerr << n << ": " << node_degree[n] << "\n";
          break;
        }
        if (count >= batch_size) {
          std::cout << "Count : " << count << "\n";
          std::cout << "N : " << n << "\n";
          edge_file.close();
          part++;
          count = 0;
          std::string file_name_part = file_name + "_" + std::to_string(part);
          edge_file.open(file_name_part);
          edge_file << "Account1.id|Account2.id|type|date\n";
        }
      }
    }
  }
}

uint64_t
CreateEdgesSimple(
    uint64_t num_nodes, uint64_t upper_bound, std::string file_name) {
  //Add 1k nodes high degree nodes in this range
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> node_degree_gen(
      0, upper_bound);

  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
      0, num_nodes - 1);  // distribution in range [0, num_nodes]

  uint64_t current_num_edges = 0;
  uint64_t count = 0;
  uint64_t batch_size = 100000000;
  uint32_t part = 0;

  std::string file_name_part = file_name + "_" + std::to_string(part);
  std::ofstream edge_file(file_name_part);
  edge_file << "Account1.id|Account2.id\n";

  for (uint64_t n = 0; n < num_nodes; ++n) {
    uint64_t num_edges = node_degree_gen(rng);
    while (num_edges > 0) {
      uint64_t dest_id = uint64_t(dest_node_id(rng));
      edge_file << n << "|" << dest_id << "\n";
      --num_edges;

      count++;
      current_num_edges++;
      if (count >= batch_size) {
        std::cout << "Count : " << count << "\n";
        edge_file.close();
        part++;
        count = 0;
        std::string file_name_part = file_name + "_" + std::to_string(part);
        edge_file.open(file_name_part);
        edge_file << "Account1.id|Account2.id\n";
      }
    }
  }
  return current_num_edges;
}

uint64_t
CreateNodeDegreeSimple(
    uint64_t num_nodes, [[maybe_unused]] uint64_t upper_bound,
    [[maybe_unused]] std::vector<uint64_t>& node_degree,
    [[maybe_unused]] std::string file_name) {
#if 0
  //Add 1k nodes high degree nodes in this range
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> node_degree_gen(
      0, upper_bound);
#endif

  std::random_device random_device;
  std::mt19937 generator(random_device());
  std::uniform_int_distribution<> distribution_big_int(
      0, std::numeric_limits<int>::max());

  uint64_t current_num_edges = 0;
  uint64_t count = 0;
  uint64_t batch_size = 100000000;
  uint32_t part = 0;

  std::string file_name_part = file_name + "_" + std::to_string(part);
  std::ofstream node_file(file_name_part);
  node_file << "id|name|amount\n";

  for (uint64_t n = 0; n < num_nodes; ++n) {
    //node_degree[n] = node_degree_gen(rng);
    //current_num_edges += node_degree[n];
    node_file << n << "|";
    node_file << random_string(3) << "|";                  //firstName
    node_file << distribution_big_int(generator) << "\n";  //ammount
    count++;
    if (count >= batch_size) {
      std::cout << "Count : " << count << "\n";
      node_file.close();
      part++;
      count = 0;
      std::string file_name_part = file_name + "_" + std::to_string(part);
      node_file.open(file_name_part);
      node_file << "id|name|amount\n";
    }
  }
  //std::cout << "current num edges : " << current_num_edges <<"\n";
  return current_num_edges;
}

void
InsertEdgesSplitByEdges(
    std::string file_name, uint64_t num_nodes, uint64_t num_edges) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
      0, num_nodes - 1);  // distribution in range [0, num_nodes]

  std::uniform_int_distribution<std::mt19937::result_type> type_gen(
      0, 3);  // distribution in range [0, num_nodes]
  std::vector<std::string> types{"FriendConA", "FriendConB", "AssetConA", "AssetConB"};
  uint64_t count = 0;
  uint64_t batch_size = 100000000;
  uint32_t part = 0;
  uint64_t n = 0;

  std::string file_name_part = dir_name + "/" + file_name + "_batch_" + std::to_string(part);
  std::ofstream edge_file(file_name_part);
  edge_file << "Account1.id|Account2.id|type|date\n";
  std::cout << "Count : " << count << "\n";

  for (; n < num_edges; ++n) {
      count++;
      uint64_t src_id = uint64_t(dest_node_id(rng));
      uint64_t dest_id = uint64_t(dest_node_id(rng));
      auto tm = randomTime(
          (system_clock::now()), (system_clock::now()));

      std::string edge_type = types[type_gen(rng)]; 
      edge_file << src_id << "|" << dest_id << "|" << edge_type << "|"
                << std::put_time(&tm, "%FT%T") << "\n";
      if(edge_type == "FriendConA" || edge_type == "FriendConB") {
	      edge_file << dest_id << "|" << src_id << "|" << edge_type << "|"
			<< std::put_time(&tm, "%FT%T") << "\n";
	      count++;
      }

      if (count >= batch_size) {
        std::cout << "Count : " << count << "\n";
        edge_file.close();
        part++;
        count = 0;
        std::string file_name_part = dir_name + "/" + file_name + "_batch_" + std::to_string(part);
        edge_file.open(file_name_part);
        edge_file << "Account1.id|Account2.id|type|date\n";
      }
    }
}


int
main(int argc, char** argv) {
  llvm::cl::ParseCommandLineOptions(argc, argv);

//  uint64_t num_nodes = 100000000;   // 100M
//  uint64_t num_nodes = 2000000000;  //  increase 20x to 2B on the way to 20B
  uint64_t num_nodes = 20000000000;  //  increase 200x to 20B 
  uint64_t num_edges = 0;

  switch (GenType) {
  case Nodes: {
    if(user_num_nodes != 0){
      std::cout << "user specified num_nodes = " << user_num_nodes << std::endl; 
      num_nodes = user_num_nodes; 
    } 
    uint64_t block_size = num_nodes/2000;  // tcook: 100M block size  20B/200
    uint64_t start_ID = blockID * block_size;
    if(blockID < uint64_t{0} || blockID > 1999)
    {
      std::cout << "ERROR: block_num must be between 0 and 1999. Quitting." << std::endl; 
      exit (-1); 
    }
    std::cout << "Node Generation: start_ID = " << start_ID << " block_size = " << block_size << " block_num = " << blockID << " num_nodes = " << num_nodes << std::endl;
    
//    CreateNodeFile("NodeFile", num_nodes, 0.0001, start_ID);
    std::string fname = "NodeFile_" + std::to_string(blockID); 
    CreateNodeFile(fname, block_size, 0.0001, start_ID);
    
    break;
  }
  case NodesToInsert: {
    std::cout << "New nodes to insert : " << num_nodes_to_insert << " Starting from :" <<  start_id_insertion << "\n";			      
    std::string fname = dir_name + "/NodeFileInsert_" + std::to_string(num_nodes_to_insert) + "start_" + std::to_string(start_id_insertion); 
    CreateNodeFile(fname, num_nodes_to_insert, .0001, start_id_insertion, true);
    
    break;
  }

  case AssetConA: {
    std::cout << "AssetConA\n";
    /////ASSETConA
    /////1600000000 - (0.94*9 + 0.058*133 + 0.0001*1725 + 0.000001*13667)*100000000
    // num_edges = 1600000000;  // 1.6 B
    num_edges = 3200000000;  // tcook: doubled to 3.2 B
    std::vector<uint64_t> node_degree_assetA(num_nodes, 0);
//    CreateNodeDegreeWithMean(
//        num_nodes, num_edges, node_degree_assetA, 0.94, 8, 0.058, 130, 0.001, 1725, 0.000001, 13667, 31000);

// tcook: update for paypal 40B edges but 20B nodes, basically 200x nodes but only 2x edges: 
//    CreateNodeDegreeWithMean(
//      num_nodes, num_edges, node_degree_assetA, 0.94, 1, 0.058, 1, 0.001, 600, 0.000001, 13667, 31000);

     // tcook - new distribution for 20B - should be 4.1B edges: 
    CreateNodeDegreeWithMean(num_nodes, num_edges, node_degree_assetA, 
		    0.01, 2, 0.012, 7, 0.0003, 140, 0.000015, 4000, 8000);

   
    //CreateEdgesAsset("fileAssetA", node_degree_assetA, "AssetConA");
    CreateEdgesAssetSplitByEdges("fileAssetA", node_degree_assetA, "AssetConA");

    break;
  }
  case AssetConB: {
    std::cout << "AssetConB\n";
    ///// ASSETConB
    ////12500000000 - (0.506*28 + 0.489*221 + 0.0005*3667 + 0.00004* 15000)*100000000
    //num_edges = 12500000000;   // 12.5 B
    num_edges = 25000000000;   // tcook: 25 B
    
    std::vector<uint64_t> node_degree_assetB(num_nodes, 0);

//    CreateNodeDegreeWithMean(
//        num_nodes, num_edges, node_degree_assetB, 0.506, 28, 0.489, 221, 0.0005, 3667, 0.00004, 15000, 35000);

// tcook: update for paypal 40B edges but 20B nodes, basically 200x nodes but only 2x edges: 
//      CreateNodeDegreeWithMean(
//          num_nodes, num_edges, node_degree_assetB, 0.506, 5, 0.489, 20, 0.0005, 300, 0.000004, 3000, 2020);

    // tcook - new distribution for 20B - should be ~25.7B edges:
    CreateNodeDegreeWithMean(num_nodes, num_edges, node_degree_assetB, 
		    0.5, 2, 0.015, 12, 0.0003, 300, 0.000015, 1000, 2020);

    //CreateEdgesAsset("fileAssetB", node_degree_assetB, "AssetConB");
    CreateEdgesAssetSplitByEdges("fileAssetB", node_degree_assetB, "AssetConB");

    break;
  }
  case FriendConA: {
    std::cout << "FriendConA\n";
    /// FriendConB
    ///1800000000 - (0.89*9 + 0.108*107 + 0.000026*1667)*100000000
//  num_edges = 1800000000;  // 1.8 B
    num_edges = 3600000000;  // tcook: doubled to 3.6 B
    std::vector<uint64_t> node_degree_friendA(num_nodes, 0);
//    CreateNodeDegreeWithMean(
//        num_nodes, num_edges, node_degree_friendA, 0.89, 9, 0.108, 107,
//        0.000026, 1667, 0.0, 13667, 2020);
//      CreateNodeDegreeWithMean(
//        num_nodes, num_edges, node_degree_friendA, .6, 1, .2, 7, 0.000026, 1336, 0.000018, 1, 2020);
// tcook: update for paypal 40B edges but 20B nodes, basically 200x nodes but only 2x edges: 
//        CreateNodeDegreeWithMean(
//          num_nodes, num_edges, node_degree_friendA, 0.5, 1, 0.25, 3, 0.125, 5, .125, 1, 2020);
	  CreateNodeDegreeWithMean(num_nodes, num_edges, node_degree_friendA, 
			  0.05, 2, 0.008, 10, 0.00003, 200, .000015, 800, 2020);
    //CreateEdgesFriend("fileFriendB", node_degree_friendB, "FriendConB");
    // tcook - new distribution for 20B should total: 3.9B edges:
    CreateEdgesFriendSplitByEdges(
        "fileFriendA", node_degree_friendA, "FriendConA");

    break;
  }
  case FriendConB: {
    std::cout << "FriendConB\n";
    /// FriendConB
    ///3000000000 - (0.79*8 + 0.2*107 + 0.0014*1667)*100000000
    // num_edges = 3000000000; // 3B
    num_edges = 6000000000; // tcook: doubled to 6B
    std::vector<uint64_t> node_degree_friendB(num_nodes, 0);
//    CreateNodeDegreeWithMean(
//        num_nodes, num_edges, node_degree_friendB, 0.79, 8, 0.2, 107, 0.0014,
//        1667, 0.0, 13667, 13400);
    //CreateNodeDegreeWithMean(
      //num_nodes, num_edges, node_degree_friendB, 0.79, 1, 0.0, 100, 0.0014, 1000, 0.0, 13667, 13400);

// tcook: update for paypal 40B edges but 20B nodes, basically 200x nodes but only 2x edges: 
//    CreateNodeDegreeWithMean(
//        num_nodes, num_edges, node_degree_friendB, 0.78, 2, 0.2, 7,0.0014, 32, 0.0, 13667, 2020);
    // tcook - new distribution for 20B should total: 6.3B edges:
    CreateNodeDegreeWithMean(num_nodes, num_edges, node_degree_friendB, 
		    0.13, 2, 0.001, 10, 0.0003, 100, 0.000015, 1000, 2020);
    //CreateEdgesFriend("fileFriendB", node_degree_friendB, "FriendConB");
    CreateEdgesFriendSplitByEdges(
        "fileFriendB", node_degree_friendB, "FriendConB");

    break;
  }
  case EdgesToInsert: {
    std::cout << "EdgesToInsert\n";
    std::string fname = "EdgeFileInsert_" + std::to_string(num_edges_to_insert);
    InsertEdgesSplitByEdges(
        fname, user_num_nodes, num_edges_to_insert);

    break;
  }
  case BigNodes: {
    std::cout << "BigNodes\n";
    uint64_t num_nodes = 5200000000;
    std::vector<uint64_t> node_degree(num_nodes, 0);
    //uint64_t num_edges = CreateNodeDegreeSimple(num_nodes, uint64_t(3), node_degree, "NodeFile");
    uint64_t num_edges = CreateEdgesSimple(num_nodes, uint64_t(3), "EdgeFile");
    std::cout << "num_edges : " << num_edges << "\n";

    break;
  }

  default:
    std::cerr << "Unknown option\n";
    break;
  }

  return 0;
}

