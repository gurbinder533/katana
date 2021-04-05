#include <iostream>
#include <random>
#include <cassert>  
#include <fstream>
#include <chrono>
#include <iomanip>
#include <ctime>

using namespace std::chrono;

void CreateNodeDegreeWithMean(uint64_t num_nodes, [[maybe_unused]]uint64_t num_edges, 
[[maybe_unused]]std::vector<uint64_t>& node_degree, float prob1, uint32_t mean1, float prob2, 
uint32_t mean2, float prob3, uint32_t mean3, float prob4, uint32_t mean4, 
[[maybe_unused]]uint64_t max_degree) {


    // Number of nodes in each range
    uint64_t num_type1 = prob1 * num_nodes;     //1-99
    uint64_t num_type2 = prob2 * num_nodes;     //100-999
    uint64_t num_type3 = prob3 * num_nodes;     //1000-9999
    uint64_t num_type4 = prob4 * num_nodes;     //>=10000
    
    assert((prob1 + prob2 + prob3 + prob4) <= 1);


    /** 
     * Fill node_degree for range1
     */ 
    double lower = 1;
    double upper = (3 * mean1 - lower) + 1;
    double triangle_tip = 1; //3 * mean1 - (1 + 99);
    std::random_device dev;
    std::mt19937_64 eng;
    std::piecewise_linear_distribution<> dist({lower, triangle_tip, upper},
                                              [&](double x)
                                              {
                                                  return x == triangle_tip ? 1. : 0.;
                                              });
    uint64_t current_num_edges = 0;
    std::cout << "Range: "<< 0 << " : " <<  num_type1 << "\n";
    for (uint32_t n = 0; n < num_type1; ++n) {
      node_degree[n] = uint32_t(dist(eng));
      current_num_edges += node_degree[n];
    }

    //Add 1k nodes high degree nodes in this range
    std::mt19937 rng(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist_node_id(
        0, num_type1 - 1);  // distribution in range [0, num_nodes]
    std::mt19937 rng1(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist_type1(
        50, 99);  // distribution in range [1, 99]                                

    for (uint32_t n = 0; n < 1000; ++n) {
      uint32_t node_id = dist_node_id(rng);
      node_degree[node_id] = uint32_t(dist_type1(rng1)); 
      current_num_edges += node_degree[node_id];
    }

    std::cout << "current num edges : " << current_num_edges << " : MEAN : " << current_num_edges / num_type1 << "\n";         
    
    /** 
     * Fill node_degree for range2
     */ 
    double lower2 = 100;
    double upper2 = (3 * mean2 - lower2) + 1;
    double triangle_tip2 = 1; //3 * mean1 - (1 + 99);
    std::mt19937_64 eng2;
    std::piecewise_linear_distribution<> dist2({lower2, triangle_tip2, upper2},
                                              [&](double x)
                                              {
                                                  return x == triangle_tip2 ? 1. : 0.;
                                              });

    std::cout << "Range: "<< num_type1 << " : " <<  num_type1 + num_type2 << "\n";
    for (uint32_t n = num_type1; n < num_type1+num_type2; ++n) {
      node_degree[n] = uint32_t(dist2(eng2));
      current_num_edges += node_degree[n];
    }

    //Add 1k nodes high degree nodes in this range
    std::uniform_int_distribution<std::mt19937::result_type> dist_node_id2(
        num_type1, num_type1 + num_type2 - 1);  // distribution in range [0, num_nodes]
    std::mt19937 rng2(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist_type2(
        mean2, 999);  // distribution in range [100, 999]                                

    for (uint32_t n = 0; n < 1000; ++n) {
      uint32_t node_id = dist_node_id2(rng);
      node_degree[node_id] = uint32_t(dist_type2(rng2));
      current_num_edges += node_degree[node_id]; 
    }                                

    std::cout << "current num edges : " << current_num_edges << " : MEAN : " << current_num_edges / (num_type1+num_type2) << "\n";         

    
    /** 
     * Fill node_degree for range3
     */ 
    double lower3 = 1000;
    double upper3 = (3*mean3 - lower3) + 1;
    double triangle_tip3 = 1; //3 * mean1 - (1 + 99);
    std::mt19937_64 eng3;
    std::piecewise_linear_distribution<> dist3({lower3, triangle_tip3, upper3},
                                              [&](double x)
                                              {
                                                  return x == triangle_tip3 ? 1. : 0.;
                                              });

    std::cout << "Range: "<< num_type1 + num_type2 << " : " <<  num_type1 + num_type2 + num_type3 << "\n";
    for (uint32_t n = num_type1 + num_type2; n < num_type1 + num_type2 + num_type3; ++n) {
      node_degree[n] = uint32_t(dist3(eng3));
      current_num_edges += node_degree[n];
    }

    std::cout << "current num edges : " << current_num_edges << " : MEAN : " << current_num_edges / (num_type1+num_type2+num_type3) << "\n";         



    /** 
     * Fill node_degree for range4
     */ 
    double lower4 = 10000;
    double upper4 = (3 * mean4 - lower4) + 1;
    double triangle_tip4 = 1; //3 * mean1 - (1 + 99);
    std::mt19937_64 eng4;
    std::piecewise_linear_distribution<> dist4({lower4, triangle_tip4, upper4},
                                              [&](double x)
                                              {
                                                  return x == triangle_tip4 ? 1. : 0.;
                                              });

    std::cout << "Range: "<< num_type1 + num_type2 + num_type3 << " : " <<  num_type1 + num_type2 + num_type3 + num_type4 << "\n";
    for (uint32_t n = num_type1 + num_type2 + num_type3; n < num_type1+num_type2+num_type3+num_type4; ++n) {
      node_degree[n] = uint32_t(dist4(eng4));
      current_num_edges += node_degree[n];
    }

    // Randomly pick the highest degree node from range4 or range3 if range4 is 0
    if (num_type4 > 0) {
      std::mt19937 rng4(dev());
      std::uniform_int_distribution<std::mt19937::result_type> dist_node_id4(
          (num_type1 + num_type2 + num_type3), (num_type1 + num_type2 + num_type3 + num_type4 - 1)); 
      node_degree[dist_node_id4(rng4)] = max_degree;
      current_num_edges += max_degree;                                  
    } else {
       std::mt19937 rng3(dev());
       std::uniform_int_distribution<std::mt19937::result_type> dist_node_id3(
          (num_type1 + num_type2), (num_type1 + num_type2 + num_type3 - 1)); 
      node_degree[dist_node_id3(rng3)] = max_degree;
      current_num_edges += max_degree;         
    }
  
    std::cout << "FINAL : MEAN : " << current_num_edges / (num_nodes) << "\n";

}


///////////////////// Write to files //////////////////////////

std::string RabdomYesNo() {
  if (rand() % 2 == 0) {
    return std::string("yes");
  } else {
    return std::string("no");
  }
}

int RabdomYesNoInt() {
  if (rand() % 2 == 0) {
    return 1;
  } else {
    return 0;
  }
}

std::string random_string(std::size_t length)
{
    const std::string CHARACTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_int_distribution<> distribution(0, CHARACTERS.size() - 1);

    std::string random_string;

    for (std::size_t i = 0; i < length; ++i)
    {
        random_string += CHARACTERS[distribution(generator)];
    }

    return random_string;
}
std::string random_string()
{
    const std::string CHARACTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_int_distribution<> distribution_length(0, 50);
    std::uniform_int_distribution<> distribution(0, CHARACTERS.size() - 1);


    std::string random_string;
    std::size_t length = distribution_length(generator);
    for (std::size_t i = 0; i < length; ++i)
    {
        random_string += CHARACTERS[distribution(generator)];
    }

    return random_string;
}


template <class TimePoint>
class uniform_time_distribution{
public:
  uniform_time_distribution(TimePoint start, TimePoint end)
    : m_start(start), m_end(end),
    m_seconds(std::chrono::duration_cast<std::chrono::seconds>(end - start))
  {}

  template <class Generator>
  TimePoint operator()(Generator && g){
    std::uniform_int_distribution<std::chrono::seconds::rep> d(0, m_seconds.count());

    return m_start + std::chrono::seconds(d(g));
  }

private:
  TimePoint m_start;
  TimePoint m_end;
  std::chrono::seconds m_seconds;
};

template <class TimePoint>
std::tm randomTime(TimePoint start, TimePoint end){
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

void CreateNodeFile(std::string file_name, [[maybe_unused]]uint64_t num_nodes) {
std::ofstream node_file(file_name);
node_file << "id|userIsSuspended|userIsBlacklisted|suspended|strictSuspended|blacklisted|";
node_file << "isActiveXdata|phoneIsVerified|daysOnFile|isActive|accountStatusCode|isClosedPit|is_blacklisted|";
node_file << "blacklistedDate|sumSent|cntSent|counterparty|cntAppr|cntDecl|cntRiskDecl1|amtRiskDecl|cntRiskDecl2|sumRcvd|cntRcvd|cntDist|cntDecl2\n";


std::random_device random_device;
std::mt19937 generator(random_device());

std::uniform_int_distribution<> distribution(0, 1000);

std::uniform_int_distribution<> distribution_big_int(0,
        std::numeric_limits<int>::max());

std::mt19937 generator_double(random_device());
std::uniform_real_distribution<> distribution_double(0, 10000000000);

for (uint32_t n = 0; n < num_nodes; ++n) {
  node_file << n << "|";
  node_file << RabdomYesNo() << "|"; //useIsSuspended
  node_file << RabdomYesNo() << "|"; //userIsBlacklisted
  node_file << random_string() << "|"; //suspended
  node_file << random_string() << "|"; //strictSuspended

  node_file << RabdomYesNo() << "|"; //blacklisted
  node_file << RabdomYesNo() << "|"; //isActiveXdata
  node_file << RabdomYesNo() << "|"; //phoneIsVerified
  node_file << distribution(generator) << "|"; //daysOnFile
  node_file << RabdomYesNoInt() << "|"; //isActive
  node_file << random_string(5) << "|"; //accountStatusCode
  node_file << RabdomYesNoInt() << "|"; //isClosedPit
  node_file << RabdomYesNoInt() << "|"; //is_blacklisted

  auto tm = randomTime((system_clock::now() - hours(44 * 365 * 10)),(system_clock::now()));  

  node_file << std::put_time(&tm, "%FT%T") << "|"; //blacklistedDate
  node_file << distribution_double(generator_double) << "|"; //sumSent
  node_file << distribution_big_int(generator) << "|"; //cntSent
  node_file << distribution_big_int(generator) << "|"; //counterparty
  node_file << std::setprecision(4) << std::fixed << distribution_double(generator_double) << "|"; //cntAppr
  node_file << std::setprecision(4) << std::fixed << distribution_double(generator_double) << "|"; //cntDecl
  node_file << std::setprecision(4) << std::fixed << distribution_double(generator_double) << "|"; //cntRiskDecl1
  node_file << std::setprecision(4) << std::fixed << distribution_double(generator_double) << "|"; //amtRiskDecl
  node_file << distribution_big_int(generator) << "|"; //cntRiskDecl2
  node_file << distribution_double(generator_double) << "|"; //sumRcvd
  node_file << distribution_big_int(generator) << "|"; //cntRcvd
  node_file << distribution_big_int(generator) << "|"; //cntDist
  node_file << std::setprecision(4) << std::fixed << distribution_double(generator_double) << "|\n"; //cntDecl2
  
  }
}

void CreateEdgesAsset(std::string file_name, const std::vector<uint64_t>& node_degree, std::string edge_type) {

  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
        0, node_degree.size()- 1);  // distribution in range [0, num_nodes]

  std::ofstream edge_file(file_name);
  edge_file << "Account1.id|Account2.id|type\n"; 
  for (uint32_t n = 0; n < node_degree.size(); ++n) {
    uint64_t degree = node_degree[n];
    while(degree) {
      uint32_t dest_id = uint32_t(dest_node_id(rng));
      edge_file << n << "|" << dest_id << "|" << edge_type << "\n";
      --degree;
    }
  }
}

void CreateEdgesFriend(std::string file_name, std::vector<uint64_t>& node_degree, std::string edge_type) {

  uint32_t num_nodes = node_degree.size();
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dest_node_id(
        0, num_nodes - 1);  // distribution in range [0, num_nodes]

  std::ofstream edge_file(file_name);
  edge_file << "Account1.id|Account2.id|type\n"; 
  for (uint32_t n = 0; n < num_nodes; ++n) {
    // uint64_t degree = node_degree[n];
    while(node_degree[n] > 0) {
      uint32_t dest_id = uint32_t(dest_node_id(rng));
      while(dest_id == n) {
        dest_id = uint32_t(dest_node_id(rng));
      }
      if( node_degree[dest_id] > 0) {
        edge_file << n << "|" << dest_id << "|" << edge_type << "\n";
        edge_file << dest_id << "|" << n << "|" << edge_type << "\n";
        node_degree[n]--;
        node_degree[dest_id]--;
      } else {
        uint32_t next_dest = dest_id;
        while(node_degree[next_dest] == 0) {
          if (next_dest == num_nodes - 1) {
            next_dest = 0;
          } else {
            next_dest++;
          }
        }
        edge_file << n << "|" << next_dest << "|" << edge_type << "\n";
        edge_file << next_dest << "|" << n << "|" << edge_type << "\n";
        node_degree[n]--;
        node_degree[next_dest]--;
        }
      }
  }
}

int
main() {


/////ASSETConA
/////1600000000 - (0.94*9 + 0.058*133 + 0.0001*1725 + 0.000001*13667)*100000000

///// ASSETConB
////12500000000 - (0.506*28 + 0.489*221 + 0.0005*3667 + 0.00004* 15000)*100000000


uint64_t num_nodes = 100000000;
uint64_t num_egdes = 0;
#if 0
num_egdes = 1600000000;
std::vector<uint64_t> node_degree_assetA(num_nodes, 0);

// CreateNodeFile("NodeFile", num_nodes);

CreateNodeDegreeWithMean(num_nodes, num_egdes, node_degree_assetA, 0.94, 8, 0.058, 130, 0.001, 1725, 0.000001, 13667, 31000);
  
CreateEdgesAsset("fileAssetA", node_degree_assetA, "AssetConA");


num_egdes = 12500000000;
std::vector<uint64_t> node_degree_assetB(num_nodes, 0);
CreateNodeDegreeWithMean(num_nodes, num_egdes, node_degree_assetB, 0.506, 28, 0.489, 221, 0.0005, 3667, 0.00004, 15000, 35000);
CreateEdgesAsset("fileAssetB", node_degree_assetB, "AssetConA");
#endif
/////////////// Friend connection /////////////
/// FriendConB
///3000000000 - (0.79*8 + 0.2*107 + 0.0014*1667)*100000000
num_egdes = 3000000000;
std::vector<uint64_t> node_degree_friendB(num_nodes, 0);
CreateNodeDegreeWithMean(num_nodes, num_egdes, node_degree_friendB, 0.79, 8, 0.2, 107, 0.0014, 1667, 0.0, 13667, 13400);

CreateEdgesAsset("fileFriendB", node_degree_friendB, "FriendConB");



  return 0;
}