#include <cassert>
#include <cstdint>
#include <utility>
#include <iostream>
enum EpdRotation { EPD_ROT_LANDSCAPE, EPD_ROT_PORTRAIT, EPD_ROT_INVERTED_LANDSCAPE, EPD_ROT_INVERTED_PORTRAIT };
EpdRotation display_rotation;
struct Coord_xy { uint16_t x,y; };
void _swap_int(uint16_t &x,uint16_t &y){std::swap(x,y);}
int epd_width(){return 960;} int epd_height(){return 540;}
// INJECT_EPD_ROTATE
struct Driver {
 EpdRotation rotation_;
 // INJECT_TOUCH
};
int main(){
 // For every physical GT911 pixel, all four logical rotations must address
 // the same native framebuffer pixel. Uses the pinned epdiy transform itself.
 for(int rot=0;rot<4;++rot){
  Driver d{(EpdRotation)rot};display_rotation=(EpdRotation)rot;
  for(int raw_x=0;raw_x<540;++raw_x)for(int raw_y=0;raw_y<960;++raw_y){
   int x=raw_x,y=raw_y;assert(d.transform_touch_(x,y));
   bool portrait=rot==EPD_ROT_PORTRAIT||rot==EPD_ROT_INVERTED_PORTRAIT;
   assert(x>=0&&x<(portrait?540:960)&&y>=0&&y<(portrait?960:540));
   auto native=_rotate(x,y);assert(native.x==raw_y&&native.y==539-raw_x);
  }
  for(auto xy:{std::pair{-1,0},{0,-1},{540,0},{0,960}}){int x=xy.first,y=xy.second;assert(!d.transform_touch_(x,y));}
 }
 std::cout<<"PASS: all touch coordinates match all four epdiy rotations\n";
}
