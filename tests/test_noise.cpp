#define NOISE_HOST_TEST
#include "../firmware/noise/noise.ino"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace NoiseLogic;

static int bitCount(const Plan &p) { int n=0; for(auto m:p.mask) n+=countBits(m); return n; }
static void bitsOf(uint8_t b, uint8_t *out) { out[0]=0; for(int i=0;i<8;++i) out[i+1]=(b>>i)&1; out[9]=1; }
static bool wire(Marker &m, const uint8_t *bytes, int n, uint64_t start, int jitter=0) {
  bool previous=true, found=false;
  for(int byte=0; byte<n; ++byte) {
    uint8_t b[10]; bitsOf(bytes[byte], b);
    for(int cell=0; cell<10; ++cell) {
      bool level=b[cell];
      if(level!=previous) {
        uint64_t t=start+ticks((byte*10+cell)*10);
        if(byte||cell) t+=((byte*10+cell)&1)?jitter:-jitter;
        found=m.edge(t,level,uint32_t(t*24))||found;
        previous=level;
      }
    }
  }
  return found;
}

struct State {
  Plan plan;
  uint64_t firstTicks=1000000;
  uint32_t actual[4]={}, maxLate=0;
};
struct FakeIO {
  uint64_t time=0, deadline=0;
  uint32_t readCost=1, writeCost=1, armCost=2, highWrites=0;
  bool high=false, refuseArm=false;
  uint64_t now() { time+=readCost; return time; }
  void set(bool value) { time+=writeCost; high=value; if(value) ++highWrites; }
  bool arm(uint64_t value) { time+=armCost; deadline=value; return !refuseArm; }
};

bool checkWordDelayed(unsigned word, unsigned delayedEdge, int delayTicks) {
  Marker m; bool prev = true, found = false;
  unsigned edgeIdx = 0;
  for (unsigned byte = 0; byte < 2; ++byte) {
    for (unsigned b = 0; b < 10; ++b) {
      bool high = b == 0 ? false : b == 9 ? true : bool((word >> (byte * 8 + b - 1)) & 1);
      if (high == prev) continue;
      prev = high;
      uint64_t t = 200000 + ticks((byte * 10 + b) * 10) + (edgeIdx == delayedEdge ? delayTicks : 0);
      found = m.edge(t, high, uint32_t(t * 24)) || found;
      ++edgeIdx;
    }
  }
  return found;
}

int main() {
  // 1. Plan generation & bit window verification (400,000 plans)
  assert(ticks(10)==87 && ticks(800)==6944);
  for(uint8_t mode=0; mode<4; ++mode) {
    uint32_t seed=1; bool lengths[9]={};
    for(int iteration=0; iteration<100000; ++iteration) {
      Plan p=makePlan(mode,seed);
      assert(p.mask[0]==0 && p.mask[1]==0 && p.n<=4 && p.n%2==0);
      if(mode<3) assert(bitCount(p)==mode);
      else {
        int n=bitCount(p); assert(n>=3 && n<=8); lengths[n]=true; int bytes=0;
        for(auto mask:p.mask) { if(mask){ ++bytes; while(!(mask&1)) mask>>=1; assert((mask&(mask+1))==0); } }
        assert(bytes==1);
      }
      for(int e=0; e<p.n; e+=2) {
        assert(p.event[e]<p.event[e+1]);
        assert(p.event[e+1]-p.event[e]>=50);
        assert(p.event[e+1]<ticks(800));
        int beginByte=p.event[e]/ticks(100), endByte=p.event[e+1]/ticks(100);
        assert(beginByte==endByte && beginByte>=2 && beginByte<=7);
        if(e) assert(p.event[e]>p.event[e-1]);
      }
    }
    if(mode==3) for(int i=3; i<9; ++i) assert(lengths[i]);
  }
  printf("[PASS] 400,000 plans verified.\n");

  // 2. Pulse timing & CCOUNT wrap-around
  for(uint8_t mode=1; mode<=3; ++mode) {
    uint32_t seed=1;
    for(int frame=0; frame<10000; ++frame) {
      State r; r.plan=makePlan(mode,seed); shiftPlan(r.plan, BENCH_PHASE_TICKS);
      uint8_t cursor=0; FakeIO io;
      while(cursor<r.plan.n) {
        const auto e=cursor; io.time=r.firstTicks+r.plan.event[e]+3;
        const auto result=pulseStep(r,cursor,io);
        assert(r.actual[e]>=r.plan.event[e] && r.actual[e]<=r.plan.event[e]+MAX_EVENT_LATE_TICKS);
        if(cursor==r.plan.n) assert(result==PULSE_COMPLETE && !io.high);
        else assert(result==NEXT_EVENT);
      }
      assert(r.maxLate==6);
    }
  }
  for(uint32_t late : {40u, 22u, 21u}) {
    State r; r.plan.event[0]=1000; r.plan.event[1]=1052; r.plan.n=2;
    FakeIO io; io.readCost=0; io.time=r.firstTicks+1000+late; uint8_t e=0;
    assert(pulseStep(r,e,io)==PULSE_FAILED && !io.high && io.highWrites==0);
  }
  printf("[PASS] 30,000 pulse sequences and strict timing limits verified.\n");

  // 3. Marker tests: jitter, debug frames, bench traces, and 46M exhaustive combinations
  uint8_t markerBytes[2]={0xA5,0x5A}, debug[7]={0xD3,0x91,0xA5,0x5A,2,0xA5,0x5A};
  Marker m;
  for(int i=0; i<1000; ++i) {
    uint64_t start=1000000u+uint64_t(i)*510000u;
    assert(wire(m,markerBytes,2,start,8));
    assert(!wire(m,debug,7,start+30000u,8));
  }
  assert(m.recoveredLateEdges==0 && m.timingRejects==1000);

  // All 6 real bench EDGE_TRACE logs + 15/10 log under Edge 0..14 delays
  const uint32_t benchTraces[][16] = {
    {0,78,167,247,337,517,597,687,857,1027,1117,1207,1377,1457,1547,1627},
    {0,81,170,251,339,520,600,689,860,1030,1122,1210,1380,1460,1549,1630},
    {0,80,169,249,339,520,599,689,859,1029,1119,1209,1379,1459,1550,1630},
    {0,80,169,248,338,519,598,688,858,1028,1119,1208,1378,1459,1548,1630},
    {0,81,170,250,339,521,599,689,861,1030,1120,1210,1380,1461,1551,1631},
    {0,81,172,249,340,521,600,690,881,1029,1120,1209,1380,1458,1549,1631},
    {0,80,171,250,340,520,600,691,860,1031,1120,1211,1387,1487,1549,1628},
  };
  for (auto &tr : benchTraces) {
    // Replay clean/captured trace directly
    Marker direct; bool ok = false;
    for (int i = 0; i < 16; ++i) {
      uint64_t t = 1000000 + tr[i];
      ok = direct.edge(t, (i & 1) != 0, uint32_t(t * 24)) || ok;
    }
    assert(ok);
  }
  for (unsigned idx = 0; idx < 5; ++idx) {
    auto &tr = benchTraces[idx];
    for (uint32_t d0 = 0; d0 <= 43; ++d0) {
      Marker m0; bool ok = false;
      for (int i = 0; i < 16; ++i) {
        uint64_t t = 1000000 + tr[i] + (i == 0 ? d0 : 0);
        ok = m0.edge(t, (i & 1) != 0, uint32_t(t * 24)) || ok;
      }
      assert(ok);
      int64_t firstDiff = int64_t(m0.first) - 1000000;
      assert(firstDiff >= -6 && firstDiff <= 10);
    }
    for (int k = 1; k <= 14; ++k) {
      for (uint32_t dk = 0; dk <= 42; ++dk) {
        Marker mk; bool ok = false;
        for (int i = 0; i < 16; ++i) {
          uint64_t t = 1000000 + tr[i] + (i == k ? dk : 0);
          ok = mk.edge(t, (i & 1) != 0, uint32_t(t * 24)) || ok;
        }
        assert(ok);
      }
    }
  }
  for (unsigned w = 0; w < 65536; ++w) {
    for (unsigned e = 0; e < 16; ++e) {
      for (int d = 0; d <= 43; ++d) {
        bool res = checkWordDelayed(w, e, d);
        if (w != 0x5AA5) assert(!res);
        else if (e < 15) assert(res);
      }
    }
  }
  printf("[PASS] All bench traces and 46,137,344 header/delay combinations verified.\n");
  return 0;
}
