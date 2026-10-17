#include "../nx_perf_csv.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <limits>
int main() {
 auto dir=std::filesystem::temp_directory_path()/("liberty-perf-test-"+std::to_string(getpid()));
 std::filesystem::create_directory(dir);
 { std::ofstream f(dir/"perf-0001.csv"); f<<"previous result\n"; }
 std::string path;
 {
  theft4::PerfCsv csv;assert(csv.Open(dir.c_str()));path=csv.path();assert(path.ends_with("perf-0002.csv"));
  theft4::PerfSample s{10.125,10.125,5,25000,20,9000,750,2,1,0,0,0,0,0,0};
  assert(csv.Write(s));s.window_s=0;assert(!csv.Write(s));
  s.window_s=std::numeric_limits<double>::quiet_NaN();assert(!csv.Write(s));
 }
 std::ifstream f(path);std::string h,row,extra;std::getline(f,h);std::getline(f,row);
 assert(h.starts_with("elapsed_s,window_s,frames,"));
 assert(row=="10.125000,10.125000,5,25000,20,9000.000000,750.000000,2.000000,1.000000,0,0.000000,0,0.000000,0,0");
 assert(!std::getline(f,extra));
 std::ifstream old(dir/"perf-0001.csv");std::getline(old,row);assert(row=="previous result");
 theft4::PerfCsv absent;assert(!absent.Open((dir/"missing").c_str()));
 f.close();old.close();std::filesystem::remove_all(dir);
}
