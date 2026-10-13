#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace lumashot {
// Pure state for the pin clothesline: layout, rope shape and spring physics.
// No windows, timers or rendering; every length is in physical pixels of the
// clothesline window and `scale` is its DPI / 96. Step() advances one frame
// (k = elapsed / 16.667 ms) and reports whether anything is still moving, so
// the owner can stop its timer as soon as the rope settles.
struct ClotheslineRect {
    float x{},y{},w{},h{};
    bool Contains(float px,float py)const{return px>=x&&py>=y&&px<x+w&&py<y+h;}
};
struct ClotheslineItem {
    uint64_t id{};
    float image_w{},image_h{};   // full pin image, physical pixels
    float tw{},th{};             // thumbnail picture size (without the paper border)
    float x{},tx{},vx{};         // clip position along the rope, target and velocity
    float hx{};                  // sideways pull while held
    float dy{},vdy{};            // local rope dip at the clip
    float theta{},omega{};       // swing angle (radians, clockwise) and angular velocity
    float fly{1},fly_ms{300};    // arrival progress 0..1
    ClotheslineRect from{};      // arrival start rectangle
    float pull_x{},pull_y{};     // pointer offset while held
    bool held{};
};
struct ClotheslineGhost { float x{},dy{},v{}; };

class ClotheslineModel {
public:
    static constexpr float MaxThumbW=130,MaxThumbH=92,Gap=30,Pad=5,HangZone=150,ShownTop=58,Margin=26;
    float scale{1},width{1200};
    float rope_y{-182},rope_v{},sag{16},sag_v{};
    bool visible{};
    std::vector<ClotheslineItem> items;
    std::vector<ClotheslineGhost> ghosts;
    std::optional<int> preview;float preview_w{},preview_h{};ClotheslineRect preview_rect{};

    float Left()const{return Margin*scale;}
    float Right()const{return std::max(Left()+1,width-Margin*scale);}
    float ShownY()const{return ShownTop*scale;}
    float HiddenY()const{return -(MaxThumbH+90)*scale;}
    float WindowHeight()const{return (ShownTop+MaxThumbH+150)*scale;}
    float Border()const{return Pad*scale;}
    bool Settled()const{return !visible&&rope_y<=HiddenY()+1&&ghosts.empty();}
    // Lowest window row anything on the rope can reach (rope sag, clips and the
    // thumbnails swung to any angle around their pivot); conservative.
    float ContentBottom()const{
        float bottom=rope_y+std::max(0.f,sag);
        for(const auto& g:ghosts)bottom=std::max(bottom,rope_y+std::max(0.f,sag)+std::max(0.f,g.dy));
        for(const auto& it:items){
            const auto r=FrameRect(it);float px,py;Pivot(it,px,py);
            const float dx=std::max(std::abs(r.x-px),std::abs(r.x+r.w-px)),dy=std::max(std::abs(r.y-py),std::abs(r.y+r.h-py));
            bottom=std::max(bottom,py+std::sqrt(dx*dx+dy*dy));
        }
        return bottom;
    }

    void Thumb(float w,float h,float& tw,float& th)const{
        const float s=std::min({MaxThumbW*scale/std::max(1.f,w),MaxThumbH*scale/std::max(1.f,h),1.f});
        tw=std::max(4.f,w*s);th=std::max(4.f,h*s);
    }
    // Influence of a point load at xi on the rope at x: a taut string pulled at one
    // point forms straight segments to both anchors.
    float Hat(float x,float xi)const{
        const float l=Left(),r=Right();
        if(x<=xi)return std::clamp((x-l)/std::max(1.f,xi-l),0.f,1.f);
        return std::clamp((r-x)/std::max(1.f,r-xi),0.f,1.f);
    }
    float RopeY(float x)const{
        const float l=Left(),r=Right(),t=std::clamp((x-l)/(r-l),0.f,1.f);
        float y=rope_y+sag*4*t*(1-t);
        for(const auto& it:items)if(it.fly>=1)y+=it.dy*Hat(x,it.x+it.hx);
        for(const auto& g:ghosts)y+=g.dy*Hat(x,g.x);
        return y;
    }
    int InsertIndex(float x)const{int i=0;for(const auto& it:items)if(it.x<x)++i;return i;}
    int Find(uint64_t id)const{for(size_t i=0;i<items.size();++i)if(items[i].id==id)return int(i);return -1;}

    void Layout(){
        struct Slot{ClotheslineItem* item;float w,h;};
        std::vector<Slot> seq;seq.reserve(items.size()+1);
        const float b=2*Border();
        for(auto& it:items){Thumb(it.image_w,it.image_h,it.tw,it.th);seq.push_back({&it,it.tw+b,it.th+b});}
        if(preview)seq.insert(seq.begin()+std::clamp(*preview,0,int(items.size())),Slot{nullptr,preview_w+b,preview_h+b});
        if(seq.empty())return;
        const float gap=Gap*scale;float total=-gap;for(const auto& s:seq)total+=s.w+gap;
        const float avail=Right()-Left()-110*scale;
        const float f=total>avail&&total>0?std::max(.4f,avail/total):1.f;
        if(f<1){for(auto& it:items){it.tw*=f;it.th*=f;}for(auto& s:seq){s.w*=f;s.h*=f;}total*=f;}
        float x=(Left()+Right())/2-total/2;
        for(const auto& s:seq){
            const float c=x+s.w/2;
            if(!s.item)preview_rect={c-s.w/2,RopeY(c)+3*scale,s.w,s.h};else s.item->tx=c;
            x+=s.w+gap*f;
        }
    }
    // Unrotated paper rectangle (picture + border) in window coordinates.
    ClotheslineRect FrameRect(const ClotheslineItem& it)const{
        const float ax=it.x+it.hx,ay=RopeY(ax),w=it.tw+2*Border(),h=it.th+2*Border();
        const ClotheslineRect goal{ax-w/2,ay+3*scale,w,h};
        if(it.fly>=1)return goal;
        const float p=std::clamp(it.fly,0.f,1.f),e=1-std::pow(1-p,3.f);
        const float arc=-std::sin(p*3.14159265f)*40*scale;
        return {it.from.x+(goal.x-it.from.x)*e,it.from.y+(goal.y-it.from.y)*e+arc*(1-e*.3f),
                it.from.w+(goal.w-it.from.w)*e,it.from.h+(goal.h-it.from.h)*e};
    }
    void Pivot(const ClotheslineItem& it,float& px,float& py)const{px=it.x+it.hx;py=RopeY(px)+scale;}
    // Thumbnail under (x, y), including the clip above it; `margin` widens the
    // paper rectangle on every side (used to decide whether the rope stays open).
    int Hit(float x,float y,float margin=0)const{
        for(int i=int(items.size())-1;i>=0;--i){
            const auto& it=items[size_t(i)];if(it.fly<1)continue;
            float px,py;Pivot(it,px,py);
            const float c=std::cos(-it.theta),s=std::sin(-it.theta);
            const float lx=(x-px)*c-(y-py)*s+px,ly=(x-px)*s+(y-py)*c+py;
            auto r=FrameRect(it);r.x-=margin;r.w+=2*margin;r.y-=17*scale+margin;r.h+=17*scale+2*margin;
            if(r.Contains(lx,ly))return i;
        }
        return -1;
    }
    bool OnRope(float x,float y)const{return x>=Left()-8*scale&&x<=Right()+8*scale&&std::abs(y-RopeY(x))<=7*scale;}
    void Add(ClotheslineItem item,int index){
        Thumb(item.image_w,item.image_h,item.tw,item.th);
        if(item.fly<1&&item.from.w<=0)item.fly=1;
        items.insert(items.begin()+std::clamp(index,0,int(items.size())),item);
        Layout();
    }
    std::optional<ClotheslineItem> Remove(uint64_t id){
        const int i=Find(id);if(i<0)return std::nullopt;
        auto it=items[size_t(i)];items.erase(items.begin()+i);
        if(it.fly>=1)ghosts.push_back({it.x+it.hx,it.dy,it.vdy-4*scale});
        sag_v-=1.5f*scale;
        for(auto& o:items)if(std::abs(o.x-it.x)<260*scale)o.omega+=(o.x<it.x?-1.f:1.f)*0.03f;
        Layout();
        return it;
    }
    void Wind(float x,float y,float vx){
        for(auto& it:items){
            if(it.fly<1||it.held)continue;
            const float ay=RopeY(it.x);
            if(std::abs(x-it.x)<it.tw/2+8*scale&&y>ay+it.th*.35f&&y<ay+it.th+20*scale)
                it.omega-=std::clamp(vx/scale*0.0022f,-0.035f,0.035f);
        }
    }
    void Kick(float amount){
        for(auto& it:items){const float r=float((it.id*2654435761ull>>8)%200)/100.f-1.f;it.omega+=amount*(r==0?.5f:r);}
    }
    bool Step(float k){
        k=std::clamp(k,0.f,2.5f);
        const float goal=visible?ShownY():HiddenY();
        rope_v+=((goal-rope_y)*0.055f-rope_v*0.17f)*k;rope_y+=rope_v*k;
        const float sag_goal=(16+std::min(float(items.size()),12.f)*2.6f)*scale;
        sag_v+=((sag_goal-sag)*0.08f-sag_v*0.12f)*k;sag+=sag_v*k;
        Layout();
        bool active=std::abs(goal-rope_y)>.5f||std::abs(rope_v)>.05f||std::abs(sag_goal-sag)>.3f||std::abs(sag_v)>.05f;
        for(auto& it:items){
            if(it.fly<1){
                it.fly=std::min(1.f,it.fly+k*16.667f/std::max(1.f,it.fly_ms));active=true;
                if(it.fly>=1){it.vdy=6.5f*scale;it.omega+=float(int(it.id%3)-1)*0.06f+0.03f;}
            }
            const float ax=((it.tx-it.x)*0.12f-it.vx*0.32f)*k;
            it.vx+=ax;it.x+=it.vx*k;it.omega-=ax/scale*0.006f;
            if(it.held){
                const float tdy=it.pull_y>0?85*scale*std::tanh(it.pull_y/(120*scale)):it.pull_y*0.2f;
                const float blend=std::min(1.f,0.4f*k);
                it.dy+=(tdy-it.dy)*blend;it.vdy=0;
                it.hx+=(46*scale*std::tanh(it.pull_x/(110*scale))-it.hx)*blend;
            }else{
                it.vdy+=(-it.dy*0.09f-it.vdy*0.11f)*k;it.dy+=it.vdy*k;
                it.hx*=std::pow(0.82f,k);
            }
            it.omega+=(-0.016f*std::sin(it.theta)-it.omega*0.035f)*k;
            it.theta=std::clamp(it.theta+it.omega*k,-0.75f,0.75f);
            if(it.held||std::abs(it.tx-it.x)>.3f||std::abs(it.vx)>.05f||std::abs(it.dy)>.2f||std::abs(it.vdy)>.05f||
               std::abs(it.theta)>.003f||std::abs(it.omega)>.0005f||std::abs(it.hx)>.3f)active=true;
        }
        for(auto& g:ghosts){g.v+=(-g.dy*0.09f-g.v*0.1f)*k;g.dy+=g.v*k;}
        std::erase_if(ghosts,[](const ClotheslineGhost& g){return std::abs(g.dy)<.08f&&std::abs(g.v)<.08f;});
        // Folded away and entirely above the window (with a margin for shadows
        // and the hooks): nothing on the rope can be seen, so the rest of the
        // swing is dropped instead of being animated (and rendered) off screen
        // for seconds.
        if(!visible&&ghosts.empty()&&ContentBottom()+24*scale<0&&
           std::none_of(items.begin(),items.end(),[](const ClotheslineItem& it){return it.fly<1||it.held;})){
            Rest(sag_goal);return false;
        }
        return active||!ghosts.empty();
    }
    // Final resting state of a folded rope.
    void Rest(float sag_goal){
        rope_y=HiddenY();rope_v=0;sag=sag_goal;sag_v=0;
        for(auto& it:items){it.x=it.tx;it.vx=0;it.hx=0;it.dy=0;it.vdy=0;it.theta=0;it.omega=0;}
    }
};
}