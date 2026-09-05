// Methods marked below are injected from the actual board driver by test_driver.py.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <iostream>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
enum EpdDrawMode { MODE_GC16=2, MODE_DU=1, MODE_PACKING_1PPB_DIFFERENCE=256 };
enum EpdDrawError { EPD_DRAW_SUCCESS, EPD_DRAW_FAILED };
constexpr int ESP_OK=0;
struct State {
 uint8_t front[4]={}, back[4]={}, difference[8]={};
 uint8_t *back_fb=back, *difference_fb=difference; void* waveform=nullptr;
};
uint32_t now=100;
uint32_t millis(){return now;}
int powerups=0,poweroffs=0,draws=0,cleans=0,solid_calls=0,fail_solid_at=0;
std::vector<int> transitions;
bool rail=false,fail_draw=false,fail_clean=false,fail_poweron=false,fail_poweroff=false;
uint8_t physical[4]={255,255,255,255};
std::vector<int> power_results;
int epd_poweron_checked(){++powerups;rail=!fail_poweron;return fail_poweron?-1:ESP_OK;}
int epd_poweroff_checked(){++poweroffs;rail=false;return fail_poweroff?-1:ESP_OK;}
int epd_full_screen(){return 0;}
int i2c_master_transmit_receive(void*,uint8_t*,int,uint8_t*val,int,int){
 int result=power_results.empty()?1:power_results.front();
 if(!power_results.empty())power_results.erase(power_results.begin());
 *val=result==1?0x40:0;return result<0?-1:ESP_OK;
}
EpdDrawError epd_draw_base(int,uint8_t*data,int,EpdDrawMode mode,int,void*,void*,void*){
 assert(rail && (mode==(MODE_DU|MODE_PACKING_1PPB_DIFFERENCE) || mode==(MODE_GC16|MODE_PACKING_1PPB_DIFFERENCE)));
 for(int i=0;i<8;++i)assert(data[i]==data[0]);
 assert(data[0]==0xf0 || data[0]==0x0f);
 transitions.push_back(data[0]); ++solid_calls;
 if (mode==(MODE_DU|MODE_PACKING_1PPB_DIFFERENCE)) ++cleans;
 if(fail_clean || solid_calls==fail_solid_at){physical[0]=42;return EPD_DRAW_FAILED;}
 memset(physical,data[0]==0xf0?255:0,4);return EPD_DRAW_SUCCESS;
}
EpdDrawError epd_hl_update_screen(State*s,EpdDrawMode,int){
 assert(rail);
 if(memcmp(s->front,s->back,4)==0)return EPD_DRAW_SUCCESS;
 ++draws;
 // Pinned epdiy advances back even after draw errors. Model that exact hazard.
 memcpy(s->back,s->front,4);
 if(fail_draw){physical[0]=42;return EPD_DRAW_FAILED;}
 memcpy(physical,s->front,4);return EPD_DRAW_SUCCESS;
}
struct Panel {
 State hl_; uint8_t*fb_=hl_.front; size_t fb_size_=4;
 bool on_charger_=true,last_push_succeeded_=false;
 bool clean_requested_=false,deep_clean_requested_=false,retry_pending_=false;
 uint32_t last_loop_ms_=0;
 bool failed_=false; bool is_failed()const{return failed_;}
 uint32_t retry_due_=0,retry_attempts_=0,pushes_since_clean_=0;
 uint32_t draw_failures_=0,clean_count_=0;
 int batt_mv_=4000,refresh_count_=0;
 uint32_t blank_frame_count_=0,unchanged_frames_=0,pg_fail_count_=0;
 void*pca_=(void*)1;
 uint8_t image[4]={0,1,2,3};
 void do_update_(){memcpy(fb_,image,4);}
 void refresh_battery_(){}
 // INJECT_METHODS
};
int main(){
 Panel p;p.update();assert(cleans==1&&draws==1&&p.last_push_succeeded_);
 p.update();assert(powerups==1&&p.unchanged_frames_==1);
 for(int i=0;i<19;++i){++p.image[0];p.update();}
 assert(cleans==1&&p.pushes_since_clean_==19);
 // A real periodic clean must happen even if the next requested frame is unchanged.
 p.update();assert(cleans==2&&p.pushes_since_clean_==0);
 p.clean_screen();assert(cleans==3&&!p.clean_requested_);
 assert(memcmp(physical,p.image,4)==0);

 fail_draw=true;++p.image[0];p.update();
 assert(!p.last_push_succeeded_&&p.retry_pending_&&p.draw_failures_==1);
 assert(memcmp(p.hl_.back,p.image,4)==0&&memcmp(physical,p.image,4)!=0);
 const int failed_at=draws;
 now+=4999;p.update();assert(draws==failed_at);
 fail_draw=false;++now;p.update();
 assert(cleans==4&&p.last_push_succeeded_&&!p.retry_pending_);
 assert(memcmp(physical,p.image,4)==0); // Same image recovers despite advanced back buffer.

 fail_clean=true;p.clean_screen();assert(!p.last_push_succeeded_);
 const int before=draws;now+=5000;p.update();assert(draws==before);
 now+=5000;p.update();now+=5000;p.update();
 assert(p.retry_attempts_==3&&!p.retry_pending_); // no tight retry loop
 fail_clean=false;now+=60000;p.update();assert(p.last_push_succeeded_);

 power_results={-1};++p.image[0];const int n=draws;p.update();
 assert(draws==n&&p.pg_fail_count_==1&&!rail);
 now+=5000;power_results={1,0};p.update();
 assert(!p.last_push_succeeded_&&draws==n); // lost rail after white: no content pass
 now+=5000;p.update();assert(p.last_push_succeeded_);
 // Unknown power handle must fail closed too.
 p.pca_=nullptr;++p.image[0];p.update();assert(!p.last_push_succeeded_);
 p.pca_=(void*)1;now+=5000;p.update();

 p.on_charger_=false;p.batt_mv_=3400;
 int powers=powerups;p.clean_screen();assert(powerups==powers&&p.clean_requested_);
 p.batt_mv_=0;p.update();assert(powerups==powers);
 p.batt_mv_=4000;p.update();assert(!p.clean_requested_&&p.last_push_succeeded_);
 memset(p.image,255,4);powers=powerups;p.clean_screen();
 assert(powerups==powers&&p.blank_frame_count_==1&&p.clean_requested_);

 // A checked power-on failure forbids BOTH the white and content waveforms,
 // even if the independent PCA check would have looked healthy.
 p.image[0]=0;fail_poweron=true;
 int old_draws=draws,old_cleans=cleans;p.update();
 assert(draws==old_draws&&cleans==old_cleans&&!p.last_push_succeeded_&&!rail);
 fail_poweron=false;now+=5000;p.update();assert(p.last_push_succeeded_);
 fail_poweroff=true;++p.image[0];p.update();
 assert(!p.last_push_succeeded_&&p.retry_pending_);
 fail_poweroff=false;now+=5000;p.update();assert(p.last_push_succeeded_);

 // Retry timing crosses millis wrap safely.
 p.image[0]=0;now=UINT32_MAX-100;fail_clean=true;p.clean_screen();
 assert(p.retry_pending_);powers=powerups;now+=4999;p.update();assert(powerups==powers);
 ++now;fail_clean=false;p.update();assert(p.last_push_succeeded_);
 // Public deep_clean retains three GC16 cycles, preserving the first render.
 p.on_charger_=true; transitions.clear(); old_draws=draws; p.deep_clean();
 assert((transitions==std::vector<int>{0xf0,0x0f,0xf0,0x0f,0xf0,0x0f,0xf0}));
 assert(draws==old_draws+1 && p.last_push_succeeded_ && !p.deep_clean_requested_);
 assert(memcmp(physical,p.image,4)==0);
 // Failure of every solid pass aborts content and retains a recovery request.
 for(int pass=1;pass<=7;++pass){
   fail_solid_at=solid_calls+pass; old_draws=draws; p.deep_clean();
   assert(draws==old_draws && !p.last_push_succeeded_ && p.deep_clean_requested_);
   fail_solid_at=0; now+=5000;p.update();assert(p.last_push_succeeded_);
 }
 p.on_charger_=false;transitions.clear();p.deep_clean();
 assert((transitions==std::vector<int>{0xf0}));
 p.batt_mv_=0;powers=powerups;p.deep_clean();assert(powerups==powers&&p.deep_clean_requested_);
 p.batt_mv_=4000;p.update();assert(p.last_push_succeeded_);
 memset(p.image,255,4);powers=powerups;p.deep_clean();assert(powerups==powers);
 p.failed_=true;p.image[0]=0;p.update();assert(powerups==powers);
 p.failed_=false;p.fb_=nullptr;p.update();assert(powerups==powers);
 assert(powerups==poweroffs&&!rail);
 std::cout<<"PASS: real driver clean, recovery, power guards, retry limits and wraparound\n";
}
