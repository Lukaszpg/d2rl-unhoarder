#include "src/ground_label_background.hpp"
#include <cstdio>
#include <limits>
#include <bit>
int main(){
 using UnHoarder::GroundLabelBackground::KnownNativeBlack;
 for(unsigned percent=0;percent<=100;++percent)
  if(!KnownNativeBlack({0,0,0,percent/100.f}))return 1;
 for(auto bits:{1u,0x3f199999u,0x3f19999au,0x3f19999bu,0x3f4cccccu,0x3f4ccccdu,0x3f4cccceu,0x3f800000u,0x80000000u})
  if(!KnownNativeBlack({0,0,0,std::bit_cast<float>(bits)}))return 2;
 for(auto bits:{0x3f800001u,0x80000001u,0xbf800000u,0x7f800000u,0xff800000u,0x7fc00000u,0x7f800001u,0xffc00000u})
  if(KnownNativeBlack({0,0,0,std::bit_cast<float>(bits)}))return 3;
 for(float alpha:{0.f,.6f,.8f,1.f})for(unsigned channel=0;channel<3;++channel)
  for(auto rgbBits:{1u,0x3a83126fu,0x80000000u,0x7fc00000u,0x7f800000u}) {
   auto color=std::array<float,4>{0,0,0,alpha};color[channel]=std::bit_cast<float>(rgbBits);
   if(KnownNativeBlack(color))return 4;
  }
 if(KnownNativeBlack({0,.25f,.5f,1}))return 5;
 if(KnownNativeBlack({110.f/255,35.f/255,160.f/255,.82f}))return 6;
 std::puts("Native opacity 0..100% accepted; malformed alpha, nonblack RGB, selected blue and already-styled colors refused.");
}
