// Pin clothesline: model physics, the real layered rope window, and the
// PinManager hang / retrieve / detach flow. Synthetic images only; the user's
// cursor is never moved (all positions are passed in explicitly).
#include "../src/pin/pin.cpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
namespace lumashot {
namespace {
void Pump(DWORD ms){
    const auto end=GetTickCount64()+ms;MSG msg{};
    while(GetTickCount64()<end){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(5);}
}
bool Finite(const ClotheslineModel& m){
    if(!std::isfinite(m.rope_y)||!std::isfinite(m.sag))return false;
    for(const auto& it:m.items)if(!std::isfinite(it.x)||!std::isfinite(it.theta)||!std::isfinite(it.dy))return false;
    return true;
}
ClotheslineItem Item(uint64_t id,float w,float h){ClotheslineItem it;it.id=id;it.image_w=w;it.image_h=h;return it;}
void WriteBmp(const std::filesystem::path& path,const uint32_t* pixels,int w,int h){
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{sizeof(info),w,-h,1,32,BI_RGB};
    const DWORD bytes=DWORD(w)*DWORD(h)*4;
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+bytes;
    // Composite the premultiplied surface over a mid-grey "desktop" so it is viewable.
    std::vector<uint32_t> out(size_t(w)*size_t(h));
    for(size_t i=0;i<out.size();++i){
        const uint32_t c=pixels[i],a=c>>24,bg=0x60*(255-a)/255;
        auto ch=[&](int s){return std::min<uint32_t>(255,((c>>s)&255)+bg);};
        out[i]=0xff000000|(ch(16)<<16)|(ch(8)<<8)|ch(0);
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path,std::ios::binary);
    f.write(reinterpret_cast<const char*>(&file),sizeof(file));f.write(reinterpret_cast<const char*>(&info),sizeof(info));
    f.write(reinterpret_cast<const char*>(out.data()),std::streamsize(bytes));
}
}

struct ClotheslineTest {
    static void Dump(Clothesline& line,const std::filesystem::path& path){
        if(line.surface_)WriteBmp(path,line.surface_->Pixels(),line.surface_width_,line.surface_height_);
    }
    static POINT ItemCenter(const Clothesline& line,uint64_t id){
        const int i=line.model_.Find(id);if(i<0)return {};
        const auto r=line.model_.FrameRect(line.model_.items[size_t(i)]);
        return {LONG(std::lround(r.x+r.w/2)),LONG(std::lround(r.y+r.h/2))};
    }
    static bool Flying(const Clothesline& line){for(const auto& it:line.model_.items)if(it.fly<1)return true;return false;}
    static RECT Monitor(const Clothesline& line){return line.monitor_;}
    static const ClotheslineModel& Model(const Clothesline& line){return line.model_;}
    // Glow driven with explicit cursor/time (no real cursor, no message pump).
    static void Glow(Clothesline& line,POINT cursor,ULONGLONG now,const RECT& monitor,float dwell=0){line.dwell_=dwell;line.UpdateGlow(cursor,now,true,monitor);}
    static void ShowAt(Clothesline& line,POINT point){line.ShowAt(point);}
    static float GlowLevel(const Clothesline& line){return line.glow_;}
    static float TabLevel(const Clothesline& line){return line.tab_;}
    static HWND GlowWindow(const Clothesline& line){return line.glow_window_;}
    static RECT Tab(const Clothesline& line){return line.tab_screen_;}
    // A sleeping poll (cursor at rest, woken by mouse input) still watches the edge.
    static UINT PollInterval(const Clothesline& line){return line.polling_||line.sleeping_?line.poll_interval_:0;}
    static constexpr int kSleepPolls=Clothesline::kSleepAfterPolls;
    static bool Sleeping(const Clothesline& line){return line.sleeping_;}
    static bool Polling(const Clothesline& line){return line.polling_;}
    static void Still(Clothesline& line,POINT cursor,bool near_edge,int polls){for(int i=0;i<polls;++i)line.TrackStill(cursor,near_edge);}
    static void Wake(Clothesline& line){line.WakeOnInput();}
    static HWND RawMouseTarget(){
        UINT n=0;GetRegisteredRawInputDevices(nullptr,&n,sizeof(RAWINPUTDEVICE));std::vector<RAWINPUTDEVICE> d(n);
        if(n)GetRegisteredRawInputDevices(d.data(),&n,sizeof(RAWINPUTDEVICE));
        for(const auto& x:d)if(x.usUsagePage==1&&x.usUsage==2)return x.hwndTarget?x.hwndTarget:HWND(-1);
        return nullptr;}
    static int GlowHeight(const Clothesline& line){return line.glow_height_;}
    static uint32_t GlowPixel(const Clothesline& line,int x,int y){return line.glow_surface_?line.glow_surface_->Pixels()[size_t(y)*size_t(line.glow_width_)+size_t(x)]:0;}
    static void DumpGlow(Clothesline& line,const std::filesystem::path& path){if(line.glow_surface_)WriteBmp(path,line.glow_surface_->Pixels(),line.glow_width_,line.glow_height_);}
    // Frame cost of the real Render() path (Step + draw + layered update) for a
    // 4K-wide rope at 150 % with `count` swinging cards; milliseconds.
    struct Cost{double avg{},p95{},worst{};};
    static Cost RenderCost(Clothesline& line,int frames,bool force4k){
        auto& m=line.model_;const float width=m.width,scale=m.scale;
        if(force4k){m.width=3840;m.scale=1.5f;m.Layout();}
        LARGE_INTEGER f{},a{},b{};QueryPerformanceFrequency(&f);std::vector<double> t;Clothesline::FrameCost parts{};
        for(int i=0;i<frames+15;++i){
            if(i%40==0)m.Kick(.08f);
            if(!m.items.empty())m.Wind(m.items[0].x,m.RopeY(m.items[0].x)+40*m.scale,30*m.scale);
            QueryPerformanceCounter(&a);m.Step(1);line.Render();QueryPerformanceCounter(&b);
            if(i>=15){t.push_back(double(b.QuadPart-a.QuadPart)*1000/double(f.QuadPart));parts.clear+=line.cost_.clear;parts.rope+=line.cost_.rope;parts.cards+=line.cost_.cards;parts.blit+=line.cost_.blit;}
        }
        std::sort(t.begin(),t.end());Cost c;for(double v:t)c.avg+=v;c.avg/=double(t.size());
        c.p95=t[size_t(double(t.size())*.95)];c.worst=t.back();
        if(force4k){m.width=width;m.scale=scale;m.Layout();line.Render();}
        const double n=double(frames);std::cout<<"BENCH split avg: clear "<<parts.clear/n<<" rope "<<parts.rope/n<<" cards+flush "<<parts.cards/n<<" blit "<<parts.blit/n<<" ms"<<std::endl;
        return c;
    }
    // Real message-loop pacing while the cards keep swinging: timestamps of the
    // frames the rope window actually rendered (its animation messages).
    static std::vector<double> FrameTimes(Clothesline& line,DWORD ms,std::vector<double>* busy=nullptr){
        LARGE_INTEGER f{},q{};QueryPerformanceFrequency(&f);std::vector<double> times;
        auto now=[&]{QueryPerformanceCounter(&q);return double(q.QuadPart)*1000/double(f.QuadPart);};
        const double end=now()+ms;double kick=0;MSG msg{};Clothesline::FrameCost sum{};
        while(now()<end){
            if(now()>kick){line.model_.Kick(.06f);line.Animate();kick=now()+250;}
            MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
                const bool frame=msg.hwnd==line.Window()&&((msg.message==WM_TIMER&&msg.wParam==1)||msg.message==WM_APP+0x31);
                const double t0=now();if(frame)times.push_back(t0);
                TranslateMessage(&msg);DispatchMessageW(&msg);
                if(frame&&busy){
                    busy->push_back(now()-t0);const auto& c=line.cost_;sum.clear+=c.clear;sum.rope+=c.rope;sum.cards+=c.cards;sum.blit+=c.blit;
                    if(busy->back()>20)std::cout<<"  slow frame "<<busy->back()<<" ms (clear "<<c.clear<<" rope "<<c.rope<<" cards+flush "<<c.cards<<" blit "<<c.blit<<"; bakes so far rope "<<c.rope_bakes<<" sprite "<<c.sprite_bakes<<" thumb "<<c.thumb_builds<<")"<<std::endl;
                }
            }
        }
        if(!times.empty()){const double n=double(times.size());std::cout<<"BENCH paced split avg: clear "<<sum.clear/n<<" rope "<<sum.rope/n<<" cards+flush "<<sum.cards/n<<" blit "<<sum.blit/n<<" ms; bakes rope "<<line.cost_.rope_bakes<<" sprite "<<line.cost_.sprite_bakes<<" thumb "<<line.cost_.thumb_builds<<std::endl;}
        return times;
    }
    static void Hold(Clothesline& line){line.hold_until_=GetTickCount64()+600000;}
    static bool Keeps(const Clothesline& line,POINT local){return line.KeepOpen(float(local.x),float(local.y));}
    static int HintPixels(const Clothesline& line){
        if(!line.surface_)return 0;const float s=line.model_.scale;const int cx=line.surface_width_/2,y0=int(line.model_.RopeY(line.model_.width/2)+26*s),y1=int(y0+26*s);int n{};
        for(int y=std::max(0,y0);y<std::min(line.surface_height_,y1);++y)for(int x=cx-int(60*s);x<cx+int(60*s);++x)if(x>=0&&x<line.surface_width_&&(line.surface_->Pixels()[size_t(y)*size_t(line.surface_width_)+size_t(x)]>>24)>40)++n;
        return n;
    }
};

struct ClotheslineLiveTest {
    // The pin canvas plus its floating hover bar over a synthetic backdrop.
    template<class P> static void Shot(const std::string& path,P& p){
        RECT a{};GetWindowRect(p.window,&a);RECT b=a;
        const bool with_bar=p.hbar&&IsWindowVisible(p.hbar)&&p.bar_surface;
        if(with_bar)GetWindowRect(p.hbar,&b);
        const RECT u{std::min(a.left,b.left)-16,std::min(a.top,b.top)-16,std::max(a.right,b.right)+16,std::max(a.bottom,b.bottom)+16};
        const int w=u.right-u.left,h=u.bottom-u.top;std::vector<uint32_t> canvas(size_t(w)*size_t(h));
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){const uint32_t g=0x50+uint32_t(y*0x28/std::max(1,h)),bl=std::min<uint32_t>(255,g+0x18);canvas[size_t(y)*size_t(w)+size_t(x)]=0xff000000|(g<<16)|(g<<8)|bl;}
        const auto over=[&](const uint32_t* px,int sw,int sh,int ox,int oy,uint32_t alpha){
            for(int yy=0;yy<sh;++yy)for(int xx=0;xx<sw;++xx){
                const int cx=ox+xx,cy=oy+yy;if(cx<0||cy<0||cx>=w||cy>=h)continue;
                const uint32_t c=px[size_t(yy)*size_t(sw)+size_t(xx)],sa=((c>>24)*alpha)/255;uint32_t& d=canvas[size_t(cy)*size_t(w)+size_t(cx)];uint32_t result=0xff000000;
                for(int shift:{0,8,16}){const uint32_t sc=(((c>>shift)&255)*alpha)/255,dc=(d>>shift)&255;result|=std::min<uint32_t>(255,sc+dc*(255-sa)/255)<<shift;}
                d=result;
            }
        };
        over(p.surface->Pixels(),p.surface_width,p.surface_height,a.left-u.left,a.top-u.top,p.alpha);
        if(with_bar)over(p.bar_surface->Pixels(),p.bar_width,p.bar_height,b.left-u.left,b.top-u.top,255);
        WriteBmp(path,canvas.data(),w,h);
    }
    static int Run(){
        int failures{};auto expect=[&](bool ok,const char* text){std::cout<<(ok?"PASS ":"FAIL ")<<text<<std::endl;failures+=!ok;};

        // ---- Model -------------------------------------------------------
        {
            ClotheslineModel m;m.scale=1.5f;m.width=2560;m.visible=true;
            float tw{},th{};m.Thumb(1600,900,tw,th);
            expect(tw<=ClotheslineModel::MaxThumbW*m.scale+.01f&&th<=ClotheslineModel::MaxThumbH*m.scale+.01f&&std::abs(tw/th-16.f/9)<.02f,"thumbnail fits the slot and keeps aspect");
            m.Thumb(4,4000,tw,th);expect(tw>=1&&th>0,"extreme aspect keeps a visible thumbnail");
            for(uint64_t id=1;id<=3;++id)m.Add(Item(id,400,300),int(id));
            expect(m.items.size()==3&&m.items[0].tx<m.items[1].tx&&m.items[1].tx<m.items[2].tx,"items laid out left to right");
            expect(m.items[0].tx>m.Left()&&m.items[2].tx<m.Right(),"layout stays inside the rope");
            m.preview=1;m.preview_w=100;m.preview_h=60;m.Layout();
            expect(m.preview_rect.x>m.items[0].tx&&m.preview_rect.x+m.preview_rect.w<m.items[1].tx,"drag preview opens a gap at the insert index");
            m.preview.reset();m.Layout();
            int steps=0;while(m.Step(1)&&steps<4000)++steps;
            expect(steps<4000&&Finite(m),"physics settles without NaN");
            expect(std::abs(m.rope_y-m.ShownY())<1.5f,"shown rope rests at its top offset");
            const auto r=m.FrameRect(m.items[1]);
            expect(m.Hit(r.x+r.w/2,r.y+r.h/2)==1&&m.Hit(r.x+r.w/2,r.y+r.h+400)==-1,"hit test finds the hung thumbnail only");
            m.Wind(m.items[0].x,m.RopeY(m.items[0].x)+20,40);m.Kick(.2f);
            bool moved=false;for(const auto& it:m.items)moved|=std::abs(it.omega)>0;
            expect(moved,"wind and kick swing the thumbnails");
            for(int i=0;i<200;++i)m.Step(4);expect(Finite(m),"large time steps stay stable");
            auto gone=m.Remove(2);expect(gone&&m.items.size()==2&&!m.ghosts.empty(),"remove leaves a rope dip ghost");
            steps=0;while(m.Step(1)&&steps<4000)++steps;expect(m.ghosts.empty()&&Finite(m),"ghost dip relaxes");
            m.visible=false;steps=0;while(m.Step(1)&&steps<4000)++steps;
            expect(m.rope_y<=m.HiddenY()+1,"hidden rope retracts above the screen");
            expect(m.InsertIndex(-1e6f)==0&&m.InsertIndex(1e6f)==int(m.items.size()),"insert index clamps to the ends");
        }
        {
            // Folding a swinging rope: once everything is above the window the
            // swing is dropped, instead of animating (and rendering) off screen.
            ClotheslineModel m;m.scale=1.5f;m.width=3840;m.visible=true;
            for(uint64_t id=1;id<=4;++id)m.Add(Item(id,1600,900),int(id));
            int steps=0;while(m.Step(1)&&steps<4000)++steps;
            m.Kick(.3f);for(int i=0;i<20;++i)m.Step(1);
            m.visible=false;steps=0;while(m.Step(1)&&steps<4000)++steps;
            bool still=true;for(const auto& it:m.items)still&=it.theta==0&&it.omega==0&&it.dy==0;
            expect(steps<=30&&m.Settled()&&still&&m.ContentBottom()<0,"a folded swinging rope stops animating once it is off screen");
            auto arriving=Item(9,400,300);arriving.fly=0;arriving.from={100,500,400,300};m.Add(arriving,0);
            expect(m.Step(1),"a thumbnail flying to the folded rope keeps animating");
        }

        // ---- Real rope window ---------------------------------------------
        {
            std::vector<std::pair<uint64_t,RECT>> retrieved;std::vector<uint64_t> detached;
            Clothesline line({[&](uint64_t id,RECT r){retrieved.push_back({id,r});},[&](uint64_t id,POINT){detached.push_back(id);}});
            expect(!line.Window()&&!line.Visible(),"rope window is created lazily");
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);const RECT mon=mi.rcMonitor;
            auto image=std::make_shared<Frame>(MakeFrame({0,0,640,400},0xff4f8ff7));
            for(int y=0;y<400;++y)for(int x=0;x<640;++x)if((x/40+y/40)%2)image->pixels[size_t(y)*640+size_t(x)]=0xfff5c542;
            line.Hang(7,image,{mon.left+300,mon.top+300,mon.left+940,mon.top+700});
            expect(line.Window()&&IsWindowVisible(line.Window())&&line.Visible()&&line.Contains(7),"hang shows the rope with the pin");
            const LONG ex=GetWindowLongW(line.Window(),GWL_EXSTYLE);
            expect((ex&WS_EX_LAYERED)&&(ex&WS_EX_TOPMOST)&&(ex&WS_EX_NOACTIVATE)&&(ex&WS_EX_TOOLWINDOW),"rope is a layered, topmost, non-activating tool window");
            Pump(900);
            expect(!ClotheslineTest::Flying(line),"thumbnail lands on the rope");
            auto image2=std::make_shared<Frame>(MakeFrame({0,0,300,500},0xffe25c7a));
            const POINT top{mon.left+(mon.right-mon.left)/5,mon.top+10};
            expect(line.DragUpdate(top,{300,500})&&ClotheslineTest::Model(line).preview.has_value(),"drag into the top zone previews a slot");
            expect(!line.DragUpdate({top.x,mon.top+600},{300,500})&&!ClotheslineTest::Model(line).preview,"leaving the zone closes the slot");
            line.DragUpdate(top,{300,500});
            expect(line.DragDrop(8,image2,top)&&line.Contains(8)&&line.Count()==2,"drop in the zone hangs the pin");
            expect(!line.DragDrop(9,image2,top),"drop without a drag preview is ignored");
            Pump(1100);
            Pump(30);ClotheslineTest::Dump(line,"build/agent/clothesline.bmp");
            {
                const auto& md=ClotheslineTest::Model(line);const float s=md.scale;
                const auto r=md.FrameRect(md.items[size_t(md.Find(7))]);const float cx=r.x+r.w/2,lx=md.Left()+40*s;
                const POINT card{LONG(cx),LONG(r.y+r.h/2)},above{LONG(cx),LONG(md.RopeY(cx)-20*s)},under{LONG(cx),LONG(r.y+r.h+40*s)},aside{LONG(lx),LONG(md.RopeY(lx)+60*s)};
                expect(ClotheslineTest::Keeps(line,card)&&ClotheslineTest::Keeps(line,above),"the rope stays while the cursor is on a card or between the rope and the top edge");
                expect(!ClotheslineTest::Keeps(line,under)&&!ClotheslineTest::Keeps(line,aside),"the transparent space under and beside the cards does not keep the rope open");
            }
            const POINT c=ClotheslineTest::ItemCenter(line,7);
            SendMessageW(line.Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(c.x,c.y));
            SendMessageW(line.Window(),WM_LBUTTONUP,0,MAKELPARAM(c.x,c.y));
            expect(retrieved.size()==1&&retrieved[0].first==7&&!line.Contains(7),"click on a thumbnail retrieves that pin");
            if(!retrieved.empty()){const RECT r=retrieved[0].second;expect(r.right>r.left&&r.bottom>r.top&&r.top>=mon.top-200&&r.bottom<=mon.top+400,"retrieve reports the thumbnail screen rect");}
            line.RetrieveAll();Pump(700);
            expect(line.Count()==0&&retrieved.size()==2&&retrieved[1].first==8,"retrieve all takes every pin back");
            line.Hide();Pump(900);
            expect(!line.Visible(),"rope hides");
            expect(detached.empty(),"no spurious detach");
        }

        // ---- Settings switch (Clothesline::SetEnabled) ----------------------
        {
            std::vector<uint64_t> back;bool armed=true;
            Clothesline line({[&](uint64_t id,RECT){back.push_back(id);},[](uint64_t,POINT){},[&]{return armed;}});
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);const RECT mon=mi.rcMonitor;
            auto image=std::make_shared<Frame>(MakeFrame({0,0,400,260},0xff5aa0e0));
            expect(line.Enabled(),"the clothesline starts switched on");
            line.Hang(1,image,{mon.left+200,mon.top+300,mon.left+600,mon.top+560});
            line.Hang(2,image,{mon.left+700,mon.top+300,mon.left+1100,mon.top+560});
            Pump(700);
            expect(line.Count()==2&&line.Visible(),"two pins hung before switching off");
            line.SetEnabled(false);
            expect(!line.Enabled()&&!line.Visible()&&!line.GlowVisible(),"switching off folds the rope and the edge glow away");
            Pump(800);
            expect(line.Count()==0&&back.size()==2,"switching off sends every hung pin home");
            line.Hang(3,image,{mon.left+200,mon.top+300,mon.left+600,mon.top+560});
            expect(!line.Contains(3)&&!line.Visible(),"hanging is ignored while off");
            const POINT top{mon.left+(mon.right-mon.left)/3,mon.top+10};
            expect(!line.DragUpdate(top,{400,260})&&!ClotheslineTest::Model(line).preview,"dragging into the top zone shows nothing while off");
            expect(!line.DragDrop(4,image,top)&&!line.Contains(4),"dropping at the top edge is ignored while off");
            line.Restore(5,image);expect(!line.Contains(5),"session restore does not hang pins while off");
            line.Show();expect(!line.Visible(),"the rope cannot be shown while off");
            line.Arm();Pump(250);
            expect(ClotheslineTest::PollInterval(line)==0&&!line.GlowVisible(),"the top edge is not watched while off, even with pins open");
            line.SetEnabled(true);line.Arm();
            expect(line.Enabled()&&ClotheslineTest::PollInterval(line)>0,"switching back on watches the top edge again");
            line.Hang(6,image,{mon.left+200,mon.top+300,mon.left+600,mon.top+560});
            expect(line.Contains(6)&&line.Visible(),"hanging works again once switched on");
            armed=false;line.Remove(6);line.Hide();Pump(900);
        }

        // ---- Frame cost and pacing (printed; thresholds are deliberately loose) ----
        {
            Clothesline line({[](uint64_t,RECT){},[](uint64_t,POINT){}});
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);const RECT mon=mi.rcMonitor;
            for(uint64_t id=1;id<=10;++id){
                auto image=std::make_shared<Frame>(MakeFrame({0,0,1280+int(id)*40,800},0xff000000u|uint32_t(id*0x1f3a5b)));
                line.Hang(id,image,{mon.left+100+LONG(id)*60,mon.top+300,mon.left+500+LONG(id)*60,mon.top+550});
            }
            Pump(1200);ClotheslineTest::Hold(line);   // the real cursor must not fold the rope mid-measurement
            const auto real=ClotheslineTest::RenderCost(line,240,false);
            std::cout<<"BENCH frame cost "<<(mon.right-mon.left)<<" px @"<<int(ClotheslineTest::Model(line).scale*100)<<"%, 10 cards: avg "<<real.avg<<" ms, p95 "<<real.p95<<" ms, max "<<real.worst<<" ms"<<std::endl;
            const auto cost=ClotheslineTest::RenderCost(line,240,true);
            std::cout<<"BENCH frame cost 3840 px @150%, 10 cards: avg "<<cost.avg<<" ms, p95 "<<cost.p95<<" ms, max "<<cost.worst<<" ms"<<std::endl;
            // Report only: this runs on a shared desktop where background load
            // swings frame cost by 10x, so a hard threshold would be flaky.
            std::vector<double> busy;
            const auto times=ClotheslineTest::FrameTimes(line,1500,&busy);
            std::vector<double> gaps;for(size_t i=1;i<times.size();++i)gaps.push_back(times[i]-times[i-1]);
            if(gaps.size()>10){
                auto sorted=gaps;std::sort(sorted.begin(),sorted.end());const double median=sorted[sorted.size()/2];
                double mean=0,var=0;for(double g:gaps)mean+=g;mean/=double(gaps.size());for(double g:gaps)var+=(g-mean)*(g-mean);var/=double(gaps.size());
                int late=0;for(double g:gaps)late+=g>median*1.5;
                std::sort(busy.begin(),busy.end());
                std::cout<<"BENCH frame handling: median "<<busy[busy.size()/2]<<" ms, max "<<busy.back()<<" ms"<<std::endl;
                for(size_t i=0;i<gaps.size();++i)if(gaps[i]>median*1.5)std::cout<<"  late gap #"<<i<<": "<<gaps[i]<<" ms"<<std::endl;
                std::cout<<"BENCH pacing: "<<times.size()<<" frames in 1.5 s, median "<<median<<" ms, jitter sd "<<std::sqrt(var)<<" ms, max "<<sorted.back()<<" ms, late "<<late<<std::endl;
            }
            expect(times.size()>40,"swinging cards keep producing frames");
            for(uint64_t id=1;id<=10;++id)line.Remove(id);
            const auto rope=ClotheslineTest::RenderCost(line,120,true);
            std::cout<<"BENCH rope only (no cards): avg "<<rope.avg<<" ms, p95 "<<rope.p95<<" ms"<<std::endl;
            line.Hide();Pump(900);
        }

        // ---- Top-edge glow, tab and dwell bar -------------------------------
        {
            bool armed=true;
            Clothesline glow({[](uint64_t,RECT){},[](uint64_t,POINT){},[&]{return armed;}});
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);const RECT mon=mi.rcMonitor;
            glow.Arm();
            expect(glow.Window()&&ClotheslineTest::PollInterval(glow)==100,"armed by open pins: the top edge is polled every 100 ms");
            ULONGLONG t=GetTickCount64();const POINT near_top{mon.left+400,mon.top+5};
            for(int i=0;i<25;++i)ClotheslineTest::Glow(glow,near_top,t+=16,mon);
            expect(ClotheslineTest::GlowLevel(glow)>.8f&&ClotheslineTest::TabLevel(glow)>.95f&&glow.GlowVisible()&&IsWindowVisible(ClotheslineTest::GlowWindow(glow)),"near the top edge the glow and tab appear");
            const auto ex=GetWindowLongPtrW(ClotheslineTest::GlowWindow(glow),GWL_EXSTYLE);
            expect((ex&WS_EX_TRANSPARENT)&&(ex&WS_EX_LAYERED)&&(ex&WS_EX_NOACTIVATE)&&(ex&WS_EX_TOPMOST),"glow window is click-through, layered and non-activating");
            expect(ClotheslineTest::PollInterval(glow)==16,"glow frames run at 16 ms");
            const RECT tab=ClotheslineTest::Tab(glow);const LONG mid=(mon.left+mon.right)/2;
            expect(tab.right-tab.left>80&&tab.top==mon.top&&tab.left<mid&&tab.right>mid,"tab sits at the top centre");
            const int beside=int(tab.right-mon.left)+4;
            const uint32_t no_dwell=ClotheslineTest::GlowPixel(glow,beside,1)>>24;
            ClotheslineTest::Glow(glow,near_top,t+=16,mon,.6f);ClotheslineTest::DumpGlow(glow,"build/agent/clothesline-glow.bmp");
            const int gh=ClotheslineTest::GlowHeight(glow);
            expect((ClotheslineTest::GlowPixel(glow,(mon.right-mon.left)/2-200,1)>>24)>(ClotheslineTest::GlowPixel(glow,(mon.right-mon.left)/2-200,gh-2)>>24),"glow fades downwards");
            expect((ClotheslineTest::GlowPixel(glow,beside,1)>>24)>no_dwell+40,"dwell bar grows out from behind the tab");
            const float far_level=[&]{for(int i=0;i<40;++i)ClotheslineTest::Glow(glow,{near_top.x,mon.top+400},t+=16,mon);return ClotheslineTest::GlowLevel(glow);}();
            expect(far_level==0&&!glow.GlowVisible()&&!IsWindowVisible(ClotheslineTest::GlowWindow(glow)),"moving away fades the glow out and hides its window");
            expect(ClotheslineTest::PollInterval(glow)==100,"back to the 100 ms poll");
            // Idle sleep: a cursor at rest away from the edge stops the poll timer; mouse input resumes it.
            {const POINT rest{mon.left+600,mon.top+500};
                ClotheslineTest::Still(glow,rest,false,ClotheslineTest::kSleepPolls);
                const bool early=ClotheslineTest::Sleeping(glow);
                ClotheslineTest::Still(glow,rest,false,1);
                expect(!early&&ClotheslineTest::Sleeping(glow)&&!ClotheslineTest::Polling(glow)&&ClotheslineTest::RawMouseTarget()==glow.Window()&&ClotheslineTest::PollInterval(glow)==100,
                    "after 1 s at rest the poll timer stops and raw mouse input watches instead");
                ClotheslineTest::Wake(glow);
                expect(!ClotheslineTest::Sleeping(glow)&&ClotheslineTest::Polling(glow)&&ClotheslineTest::RawMouseTarget()==nullptr,"mouse input resumes the 100 ms poll and drops the raw input registration");
                ClotheslineTest::Still(glow,{rest.x,mon.top+10},true,30);
                expect(!ClotheslineTest::Sleeping(glow),"resting near the top edge never sleeps");
                POINT moving=rest;for(int i=0;i<30;++i){moving.x+=3;ClotheslineTest::Still(glow,moving,false,1);}
                expect(!ClotheslineTest::Sleeping(glow),"a moving cursor never sleeps");
                ClotheslineTest::Still(glow,rest,false,ClotheslineTest::kSleepPolls+1);const bool slept=ClotheslineTest::Sleeping(glow);
                ClotheslineTest::ShowAt(glow,{mon.left+400,mon.top+300});
                expect(slept&&!ClotheslineTest::Sleeping(glow)&&ClotheslineTest::Polling(glow)&&ClotheslineTest::RawMouseTarget()==nullptr,"showing the rope wakes the poll that hides it again");
                glow.Hide();Pump(800);
                ClotheslineTest::Still(glow,rest,false,ClotheslineTest::kSleepPolls+1);const bool slept_again=ClotheslineTest::Sleeping(glow);
                armed=false;glow.Arm();armed=true;
                expect(slept_again&&!ClotheslineTest::Sleeping(glow)&&ClotheslineTest::RawMouseTarget()==nullptr,"disarming while asleep releases the raw input registration");
                glow.Arm();
                expect(ClotheslineTest::PollInterval(glow)==100,"arming again polls every 100 ms");}
            ClotheslineTest::ShowAt(glow,{mon.left+400,mon.top+300});Pump(450);
            expect(ClotheslineTest::HintPixels(glow)>60,"empty rope shows the drop hint");
            {
                const auto& gm=ClotheslineTest::Model(glow);const float gs=gm.scale,cx=gm.width/2;
                expect(ClotheslineTest::Keeps(glow,{LONG(cx),LONG(gm.RopeY(cx)+38*gs)})&&!ClotheslineTest::Keeps(glow,{LONG(cx),LONG(gm.RopeY(cx)+130*gs)}),
                    "empty rope: the hint keeps it open, the empty space under it does not");
            }
            ClotheslineTest::Dump(glow,"build/agent/clothesline-empty.bmp");
            glow.Hide();Pump(800);armed=false;Pump(300);
            expect(ClotheslineTest::PollInterval(glow)==0&&!glow.GlowVisible(),"no open pins: the top edge is not polled");
        }

        // ---- Top-edge trigger rules (fullscreen menus, resting, clicks) ----
        {
            using F=ClotheslineEdge::Foreground;
            // Feeds 16 ms polls for `ms`; returns the time of the first fire (0 = none).
            struct Feed{ClotheslineEdge edge;ULONGLONG t{100000};float last_dwell{};bool glow{};
                ULONGLONG Run(ULONGLONG ms,F fg,bool on_tab=false,bool buttons=false){
                    const ULONGLONG start=t;ULONGLONG fired=0;
                    for(ULONGLONG e=0;e<=ms;e+=16){
                        t=start+e;
                        const auto out=edge.Update({{800,0},t,buttons,true,on_tab,fg,1.5f});
                        last_dwell=out.dwell;glow=out.glow;if(out.fire&&!fired)fired=e?e:1;
                    }
                    t+=16;return fired;}
            };
            {Feed f;const auto at=f.Run(1000,F::Normal);expect(at>=336&&at<=368,"resting still on the edge brings the rope down after 350 ms");}
            {Feed f;const auto at=f.Run(1000,F::Maximized);expect(at>=592&&at<=624,"over a maximized window the edge needs 600 ms");}
            {Feed f;const auto at=f.Run(1000,F::Maximized,true);expect(at>=336&&at<=368,"the tab itself keeps the short dwell");}
            {Feed f;const auto at=f.Run(2000,F::Fullscreen);expect(at==0&&!f.glow&&f.last_dwell==0,"fullscreen app: no glow and no dwell at the edge");}
            {Feed f;bool fired=false;POINT p{300,0};
                for(int i=0;i<120;++i){p.x+=9;fired|=f.edge.Update({p,f.t+=16,false,true,false,F::Normal,1.5f}).fire;}
                expect(!fired,"sliding along a menu bar at the edge never fills the dwell");}
            {Feed f;bool fired=false;ULONGLONG when=0;POINT p{900,0};
                for(int i=0;i<40;++i){p.x=900+((i%3)-1)*2;const auto o=f.edge.Update({p,f.t+=16,false,true,false,F::Normal,1.5f});if(o.fire&&!fired){fired=true;when=ULONGLONG(i)*16;}}
                expect(fired&&when<=368,"hand tremor of a few pixels still counts as resting");}
            {Feed f;f.Run(120,F::Normal,false,true);
                const auto at=f.Run(1500,F::Normal);
                expect(at>=720&&at<=800,"after a click at the edge the dwell waits 400 ms first");}
            {Feed f;f.Run(200,F::Normal);f.Run(50,F::Normal,false,true);const auto at=f.Run(300,F::Normal);
                expect(at==0,"pressing a button cancels a dwell in progress");}
            // Glow and tab ("晾着 N 张") only after resting on the edge, not for being near it.
            {Feed f;bool glow=false,fired=false;
                for(int i=0;i<120;++i){const POINT p{200+(i%40)*30,6+(i%5)*12};const auto o=f.edge.Update({p,f.t+=16,false,false,false,F::Normal,1.5f});glow|=o.glow;fired|=o.fire;}
                expect(!glow&&!fired,"hovering icons just below the top edge shows no glow or tab");}
            {Feed f;bool glow=false;POINT p{300,0};
                for(int i=0;i<120;++i){p.x+=9;glow|=f.edge.Update({p,f.t+=16,false,true,false,F::Normal,1.5f}).glow;}
                expect(!glow,"sliding along the edge does not reveal the tab");}
            {Feed f;bool early=false;ULONGLONG shown=0;
                for(ULONGLONG e=0;e<=320;e+=16){const auto o=f.edge.Update({{800,0},f.t+e,false,true,false,F::Normal,1.5f});if(o.glow&&!shown)shown=e?e:1;if(o.glow&&e<ClotheslineEdge::kRevealMs)early=true;}
                expect(!early&&shown>=144&&shown<=176,"a 150 ms rest on the edge reveals glow and tab before the rope");}
            {Feed f;f.Run(200,F::Normal);
                const bool kept=f.edge.Update({{800,40},f.t+=16,false,false,false,F::Normal,1.5f}).glow;
                const bool gone=f.edge.Update({{800,80},f.t+=16,false,false,false,F::Normal,1.5f}).glow;
                const bool back=f.edge.Update({{800,40},f.t+=16,false,false,false,F::Normal,1.5f}).glow;
                expect(kept&&!gone&&!back,"the tab stays within reach below the edge and hides once the cursor leaves");}
            // After a fold, a cursor parked on the edge must not bring the rope straight back.
            {Feed f;const bool first=f.Run(1000,F::Normal)!=0;f.edge.Folded();const auto again=f.Run(3000,F::Normal);
                expect(first&&again==0&&f.last_dwell==0,"a cursor still resting on the edge after the rope folds does not reopen it");
                f.edge.Update({{800,60},f.t+=16,false,false,false,F::Normal,1.5f});const auto back=f.Run(1000,F::Normal);
                expect(back>=336&&back<=368,"leaving the edge after a fold arms the dwell again");}
            {Feed f;f.Run(1000,F::Normal);f.edge.Folded();f.edge.Update({{900,0},f.t+=16,false,true,false,F::Normal,1.5f});const auto moved=f.Run(1000,F::Normal);
                expect(moved>=336&&moved<=368,"sliding along the edge away from where the rope folded arms the dwell again");}
            // Window classification (hidden test windows; no activation).
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);
            const RECT m=mi.rcMonitor,w=mi.rcWork;
            auto make=[&](DWORD style,RECT r){return CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",style,r.left,r.top,r.right-r.left,r.bottom-r.top,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);};
            const RECT inflated{m.left-8,m.top-8,m.right+8,m.bottom+8},work_inflated{w.left-8,w.top-8,w.right+8,w.bottom+8};
            HWND popup_full=make(WS_POPUP,m),popup_small=make(WS_POPUP,{m.left+100,m.top+100,m.left+900,m.top+700});
            HWND max_work=make(WS_OVERLAPPEDWINDOW|WS_MAXIMIZE,work_inflated),max_full=make(WS_OVERLAPPEDWINDOW|WS_MAXIMIZE,inflated);
            HWND normal=make(WS_OVERLAPPEDWINDOW,{m.left+100,m.top+100,m.left+900,m.top+700});
            expect(ClotheslineEdge::Classify(popup_full,m)==F::Fullscreen,"a borderless window covering the monitor is fullscreen");
            expect(ClotheslineEdge::Classify(popup_small,m)==F::Normal&&ClotheslineEdge::Classify(normal,m)==F::Normal,"ordinary windows are normal");
            expect(ClotheslineEdge::Classify(max_work,m)==F::Maximized,"a maximized window is maximized");
            expect(ClotheslineEdge::Classify(max_full,m)==F::Maximized,"maximized with an auto-hidden taskbar is still maximized, not fullscreen");
            for(HWND h:{popup_full,popup_small,max_work,max_full,normal})if(h)DestroyWindow(h);
        }

        // ---- PinManager integration --------------------------------------
        HWND host=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
        {
            PinManager manager(host);auto image=MakeFrame({0,0,480,270},0xff7ec8a0);
            manager.Create(image,image,{260,240},std::nullopt,{},false);auto& p=*manager.pins_.rbegin()->second;
            const HWND window=p.window;const auto id=p.id;
            RECT before{};GetWindowRect(window,&before);
            expect(manager.line_&&manager.line_->Window()&&ClotheslineTest::PollInterval(*manager.line_)>0,"an open pin arms the top edge");
            SendMessageW(window,WM_KEYDOWN,'H',0);
            expect(p.hung&&!IsWindowVisible(window)&&manager.line_->Contains(id)&&manager.line_->Visible(),"H hangs the pin on the rope");
            const auto snapshot=manager.SessionSnapshot();
            expect(snapshot.size()==1&&std::abs(snapshot[0].origin.x-float(before.left+p.Inset()))<1.5f,"hung pin keeps its saved position");
            Pump(700);
            manager.line_->Remove(id);
            manager.RetrievePin(id,{100,10,180,55});
            expect(!p.hung&&IsWindowVisible(window)&&p.flight.active,"retrieve shows the pin and starts the flight");
            Pump(600);
            RECT after{};GetWindowRect(window,&after);
            expect(!p.flight.active&&std::abs(p.zoom-1)<.001f&&after.left==before.left&&after.top==before.top,"flight lands at the original place and size");
            manager.HangPin(p);Pump(300);manager.line_->Remove(id);
            manager.DetachPin(id,{700,90});
            expect(!p.hung&&IsWindowVisible(window)&&p.moving&&p.dragged&&p.alpha==150&&GetCapture()==window,"pulling off the rope continues as a drag");
            ReleaseCapture();
            expect(!p.moving&&p.alpha==255,"drag end restores opacity");
            manager.HangPin(p);Pump(200);
            DestroyWindow(window);manager.Ready();
            expect(manager.line_->Count()==0&&!manager.pins_.contains(id),"closing a hung pin removes it from the rope");
            manager.line_->Hide();Pump(200);
        }
        // ---- PinManager with the clothesline switched off ------------------
        {
            bool enabled=true;PinManager manager(host);manager.clothesline_enabled=[&]{return enabled;};
            auto image=MakeFrame({0,0,480,270},0xffc07ad0);
            manager.Create(image,image,{300,260},std::nullopt,{},false);auto& p=*manager.pins_.rbegin()->second;
            const HWND window=p.window;const auto id=p.id;
            RECT before{};GetWindowRect(window,&before);
            D2D1_RECT_F on_tools[2]{};p.ToolRects(on_tools);
            const LPARAM hang_at=MAKELPARAM(int((on_tools[0].left+on_tools[0].right)/2),int((on_tools[0].top+on_tools[0].bottom)/2));
            SendMessageW(window,WM_KEYDOWN,'H',0);Pump(600);
            expect(p.hung&&manager.line_->Contains(id),"pin hung while the clothesline is on");
            enabled=false;manager.ClotheslineConfigChanged();Pump(900);
            RECT after{};GetWindowRect(window,&after);
            expect(!p.hung&&IsWindowVisible(window)&&manager.line_->Count()==0&&!manager.line_->Visible(),"switching off flies the hung pin home and folds the rope");
            expect(after.left==before.left&&after.top==before.top&&std::abs(p.zoom-1)<.001f,"the pin lands where it was hung from");
            SendMessageW(window,WM_KEYDOWN,'H',0);
            expect(!p.hung&&IsWindowVisible(window)&&!manager.line_->Contains(id),"H does nothing while off");
            D2D1_RECT_F tools[2]{};
            expect(p.ToolRects(tools)&&tools[0].right<=tools[0].left&&tools[1].right>tools[1].left,"only the close button remains while off");
            p.tracking=true;SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(40,200));Pump(260);
            p.bar_tracking=true;SendMessageW(p.hbar,WM_MOUSEMOVE,0,hang_at);
            // The bar is right-aligned: without hang, copy slides into its spot.
            expect(p.tool_alpha==1&&p.tool_hover!=PinToolHang&&p.ToolAt(hang_at)!=PinToolHang,"the old hang button spot no longer hangs while off");
            Pump(30);WriteBmp("build/agent/pin-hover-clothesline-off.bmp",p.surface->Pixels(),p.surface_width,p.surface_height);
            SendMessageW(p.hbar,WM_LBUTTONDOWN,MK_LBUTTON,hang_at);
            expect(p.tool_down!=PinToolHang,"clicking the old hang button spot does not press hang");
            ReleaseCapture();p.moving=p.selecting=false;
            p.moving=p.dragged=true;
            expect(!manager.DropOnLine(p)&&!p.hung,"dropping at the top edge does not hang while off");
            p.moving=p.dragged=false;
            enabled=true;manager.ClotheslineConfigChanged();
            expect(p.ToolRects(tools)&&tools[0].right>tools[0].left&&ClotheslineTest::PollInterval(*manager.line_)>0,"switching back on restores the hang button and the top edge");
            SendMessageW(window,WM_KEYDOWN,'H',0);
            expect(p.hung&&manager.line_->Contains(id),"H hangs again once switched on");
            DestroyWindow(window);manager.Ready();manager.line_->Hide();Pump(200);
        }
        // ---- Hover buttons on the pin (hang, close) ------------------------
        {
            PinManager manager(host);auto image=MakeFrame({0,0,480,270},0xff3a6ea5);
            manager.Create(image,image,{320,300},std::nullopt,{},false);auto& p=*manager.pins_.rbegin()->second;
            const HWND window=p.window;const auto id=p.id;
            D2D1_RECT_F tools[2]{};
            expect(p.ToolRects(tools)&&tools[0].right<tools[1].left&&tools[1].top>0,"hang and close buttons sit at the top-right of the image");
            auto at=[&](int i){return MAKELPARAM(int((tools[i].left+tools[i].right)/2),int((tools[i].top+tools[i].bottom)/2));};
            expect(p.tool_alpha==0&&p.ToolAt(at(1))<0,"buttons hidden until hovered");
            p.tracking=true;SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(40,200));Pump(260);
            expect(p.hovered&&p.tool_alpha==1,"hovering fades the buttons in");
            p.bar_tracking=true;SendMessageW(p.hbar,WM_MOUSEMOVE,0,at(1));
            expect(p.tool_hover==1&&p.ToolAt(at(1))==1,"close button highlights under the cursor");
            Pump(30);WriteBmp("build/agent/pin-hover-close.bmp",p.surface->Pixels(),p.surface_width,p.surface_height);
            SendMessageW(p.hbar,WM_MOUSEMOVE,0,at(0));Pump(30);WriteBmp("build/agent/pin-hover-hang.bmp",p.surface->Pixels(),p.surface_width,p.surface_height);
            SendMessageW(window,WM_MOUSELEAVE,0,0);SendMessageW(p.hbar,WM_MOUSELEAVE,0,0);Pump(650);
            expect(!p.hovered&&p.tool_alpha==0&&p.tool_hover<0&&!IsWindowVisible(p.hbar),"leaving the pin fades the buttons out");
            p.tracking=true;SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(40,200));Pump(260);
            SendMessageW(p.hbar,WM_LBUTTONDOWN,MK_LBUTTON,at(0));
            expect(p.tool_down==0&&GetCapture()==p.hbar&&!p.moving,"pressing a button does not start a pin drag");
            SendMessageW(p.hbar,WM_LBUTTONUP,0,at(0));
            expect(p.hung&&manager.line_->Contains(id)&&p.tool_alpha==0&&!p.hovered,"hang button hangs the pin on the rope");
            Pump(500);manager.line_->Remove(id);manager.RetrievePin(id,{100,10,180,55});Pump(600);
            p.tracking=true;SendMessageW(window,WM_MOUSEMOVE,0,at(1));Pump(260);
            p.locked=true;manager.UpdateTools(p);Pump(260);
            expect(p.tool_alpha==0&&!p.ToolsWanted()&&p.ToolAt(at(1))<0,"locked pins hide the buttons");
            p.locked=false;manager.UpdateTools(p);Pump(260);
            SendMessageW(p.hbar,WM_LBUTTONDOWN,MK_LBUTTON,at(1));SendMessageW(p.hbar,WM_LBUTTONUP,0,MAKELPARAM(5,5));
            expect(!p.closing&&IsWindowVisible(window),"releasing outside the button cancels");
            SendMessageW(p.hbar,WM_LBUTTONDOWN,MK_LBUTTON,at(1));SendMessageW(p.hbar,WM_LBUTTONUP,0,at(1));
            expect(p.closing&&IsWindow(window),"close button starts the fade-out");
            Pump(400);manager.Ready();
            expect(!IsWindow(window)&&!manager.pins_.contains(id),"the pin closes after fading out");
            manager.line_->Hide();Pump(300);
            expect(ClotheslineTest::PollInterval(*manager.line_)==0,"no pins left: the top edge is no longer polled");
        }
        // ---- Hover bar window (full.html .pin .hbar) ------------------------
        {
            PinManager manager(host);int annotated=0;
            manager.annotate=[&](uint64_t,std::shared_ptr<const Frame>,Document,RECT){++annotated;return false;};
            // A synthetic "document" so the glass bar is judged against real-looking content.
            auto image=MakeFrame({0,0,520,300},0xfff7f8fb);
            for(int y=0;y<300;++y)for(int x=0;x<520;++x){
                uint32_t& px=image.pixels[size_t(y)*520+size_t(x)];
                if(y<40)px=0xff2f6fed;else if((y-60)%28<10&&x>=28&&x<28+((y/28)%3==0?300:440))px=0xffc9ced8;
            }
            manager.Create(image,image,{320,300},std::nullopt,{},false);auto& p=*manager.pins_.rbegin()->second;
            const HWND window=p.window;const float s=p.dpi/96;
            using Bar=std::remove_reference_t<decltype(p)>::ToolBar;Bar bar{};
            expect(p.ToolLayout(bar)&&bar.order[0]==PinToolAnnotate&&bar.order[bar.count-2]==PinToolMore&&bar.order[bar.count-1]==PinToolClose,"annotate first, more then close last");
            expect(bar.count==(p.ocr_tool?6:5)&&(!p.ocr_tool||bar.order[1]==PinToolOcr),"annotate, OCR, copy, hang, more, close");
            expect(bar.rect[PinToolHang].left>bar.rect[PinToolCopy].right&&bar.rect[PinToolMore].left>bar.rect[PinToolHang].right,"copy, hang, more in order");
            expect(bar.separator>bar.rect[PinToolMore].right&&bar.separator<bar.rect[PinToolClose].left,"a separator stands before close");
            bool sized=true;for(int i=0;i<bar.count;++i){const auto& r=bar.rect[bar.order[i]];sized=sized&&std::abs((r.right-r.left)-30*s)<.01f;}
            expect(sized&&std::abs((bar.bar.bottom-bar.bar.top)-36*s)<.01f,"30 DIP buttons in a 36 DIP bar (full.html .hbar)");
            expect(!p.hbar||!IsWindowVisible(p.hbar),"no bar until hovered");
            RECT pin_rect{};GetWindowRect(window,&pin_rect);
            const float image_left=float(pin_rect.left)+float(p.Inset())+p.offset.x,image_top=float(pin_rect.top)+float(p.Inset())+p.offset.y,image_right=image_left+520*p.zoom;
            p.tracking=true;SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(int(image_left-float(pin_rect.left)+60),int(image_top-float(pin_rect.top)+200)));Pump(260);
            expect(p.tool_alpha==1&&p.hbar&&IsWindowVisible(p.hbar),"hovering the pin shows the bar window");
            if(!p.hbar){DestroyWindow(window);manager.Ready();return 1;}
            RECT bar_rect{};GetWindowRect(p.hbar,&bar_rect);p.ToolLayout(bar);
            expect(!p.bar_below&&std::abs(float(bar_rect.top)+bar.bar.bottom-(image_top-8*s))<=1.5f&&std::abs(float(bar_rect.left)+bar.bar.right-image_right)<=1.5f,"the bar floats 8 DIP above the image, right edges aligned");
            const LONG styles=GetWindowLongW(p.hbar,GWL_EXSTYLE);
            expect((styles&WS_EX_NOACTIVATE)&&(styles&WS_EX_LAYERED)&&GetWindow(p.hbar,GW_OWNER)==window,"a layered, non-activating window owned by the pin");
            {
                const int cx=int(image_right-float(pin_rect.left)-20*p.zoom),cy=int(image_top-float(pin_rect.top)+20*p.zoom);
                const uint32_t px=p.surface->Pixels()[size_t(cy)*size_t(p.surface_width)+size_t(cx)];
                expect(std::abs(int((px>>16)&255)-0x2f)<=3&&std::abs(int(px&255)-0xed)<=3,"the pin's own canvas (and so every export) has no buttons");
            }
            Shot("build/toolbar-customization/pinbar-shots/bar.bmp",p);
            const auto at=[&](int id){const auto& r=bar.rect[id];return MAKELPARAM(int((r.left+r.right)/2),int((r.top+r.bottom)/2));};
            const char* names[]={"hang","close","annotate","ocr","copy","more"};
            p.bar_tracking=true;
            for(int i=0;i<bar.count;++i){
                const int id=bar.order[i];SendMessageW(p.hbar,WM_MOUSEMOVE,0,at(id));
                expect(p.tool_hover==id&&p.ToolAt(at(id))==id&&!p.tip_ready,(std::string("hover highlights ")+names[id]).c_str());
                Pump(450);Shot(std::string("build/toolbar-customization/pinbar-shots/hover-")+names[id]+".bmp",p);
            }
            expect(p.tip_ready,"the tooltip shows after the 380 ms delay");
            SendMessageW(window,WM_MOUSELEAVE,0,0);Pump(450);
            expect(p.tool_alpha==1&&IsWindowVisible(p.hbar),"moving from the pin onto the bar keeps it shown");
            SendMessageW(p.hbar,WM_MOUSELEAVE,0,0);Pump(650);
            expect(p.tool_alpha==0&&!IsWindowVisible(p.hbar)&&p.tool_hover<0,"leaving pin and bar fades the bar out");
            expect(PinToolLabel(PinToolAnnotate,false,false,false)==L"标注"&&PinToolKey(PinToolAnnotate)==L"空格"&&PinToolLabel(PinToolHang,false,false,false)==L"晾到绳上"&&PinToolKey(PinToolHang)==L"H"
                &&PinToolLabel(PinToolMore,false,false,false)==L"更多"&&PinToolLabel(PinToolCopy,false,false,false)==L"复制图片（加入历史）","tooltips follow full.html data-tip / data-k");
            expect(PinToolLabel(PinToolCopy,false,false,true)==L"已复制 · 已加入剪贴板历史"&&PinToolLabel(PinToolOcr,false,true,false)==L"复制识别出的文字"&&PinToolLabel(PinToolOcr,true,false,false)==L"正在识别文字…","copy and OCR tooltips follow the state");
            p.tracking=true;SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(int(image_left-float(pin_rect.left)+60),int(image_top-float(pin_rect.top)+200)));Pump(260);
            {
                RECT before{};GetWindowRect(p.hbar,&before);
                SetWindowPos(window,nullptr,pin_rect.left+60,pin_rect.top+40,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);Pump(60);
                RECT after{};GetWindowRect(p.hbar,&after);
                expect(after.left-before.left==60&&after.top-before.top==40,"the bar follows the pin");
            }
            SendMessageW(p.hbar,WM_LBUTTONDOWN,MK_LBUTTON,at(PinToolAnnotate));
            expect(p.tool_down==PinToolAnnotate&&GetCapture()==p.hbar&&!p.moving,"pressing a bar button captures on the bar; the pin does not move");
            SendMessageW(p.hbar,WM_LBUTTONUP,0,at(PinToolAnnotate));
            expect(annotated==1&&!p.editing,"annotate button starts annotation (Space)");
            DestroyWindow(window);manager.Ready();Pump(100);
            // Near the top of the work area the bar drops below the image (.hbar.below).
            MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint({400,400},MONITOR_DEFAULTTONEAREST),&monitor);
            auto tiny_image=MakeFrame({0,0,int(64*s),int(36*s)},0xff52a36b);
            manager.Create(tiny_image,tiny_image,{monitor.rcWork.left+400,monitor.rcWork.top+4},std::nullopt,{},false);auto& q=*manager.pins_.rbegin()->second;
            q.tracking=true;SendMessageW(q.window,WM_MOUSEMOVE,0,MAKELPARAM(q.Inset()+5,q.Inset()+5));Pump(260);
            expect(q.ToolLayout(bar)&&bar.count==(q.ocr_tool?6:5),"tiny pins still get the whole bar (it floats outside the image)");
            if(q.hbar){
                RECT qr{},qb{};GetWindowRect(q.window,&qr);GetWindowRect(q.hbar,&qb);
                const float q_bottom=float(qr.top)+float(q.Inset())+q.offset.y+float(tiny_image.Height())*q.zoom;
                expect(q.bar_below&&std::abs(float(qb.top)+bar.bar.top-(q_bottom+8*s))<=1.5f,"no room above: the bar sits 8 DIP below the image");
                q.bar_tracking=true;const auto& more=bar.rect[PinToolMore];
                SendMessageW(q.hbar,WM_MOUSEMOVE,0,MAKELPARAM(int((more.left+more.right)/2),int((more.top+more.bottom)/2)));Pump(450);
                Shot("build/toolbar-customization/pinbar-shots/below.bmp",q);
            }else expect(false,"tiny pin shows a bar window");
            DestroyWindow(q.window);manager.Ready();Pump(100);
            manager.line_->Hide();Pump(200);
        }
        DestroyWindow(host);
        return failures?1:0;
    }
};
}
int main(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int result=1;
    try{result=lumashot::ClotheslineLiveTest::Run();}catch(const std::exception& e){std::cout<<"FAIL "<<e.what()<<std::endl;}
    CoUninitialize();std::cout<<(result?"FAILED":"ALL PASS")<<std::endl;return result;
}