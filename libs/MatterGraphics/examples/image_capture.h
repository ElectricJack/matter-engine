#pragma once
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
// Existing composition fixture image writer; the application selects a directory.
void ppm(const std::string &name, uint32_t w,uint32_t h,const std::vector<mg::Rgba> &pixels,
         const std::string &directory = "composition-evidence") {
  std::filesystem::create_directories(directory);
  std::ofstream f(directory+"/"+name+".ppm",std::ios::binary);
  f<<"P6\n"<<w<<' '<<h<<"\n255\n";
  for(const auto &p:pixels)for(int c=0;c<3;++c)f.put(char(std::lround(std::clamp(p[c],0.f,1.f)*255)));
  check(bool(f),"COMP write matched image evidence");
}
