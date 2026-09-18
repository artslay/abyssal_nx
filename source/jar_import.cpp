

#include "jar_import.h"

extern "C" void debugPrintf(const char *fmt, ...);

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <zlib.h>

namespace {

static std::string g_error;

[[noreturn]] static void fail(const std::string &s) {
  throw std::runtime_error(s);
}

static uint16_t rd16(const uint8_t *p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
static uint32_t rd32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint16_t be16(const uint8_t *p) {
  return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}
static uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static int16_t srd16(const uint8_t *p) { return static_cast<int16_t>(rd16(p)); }

static void wr16(FILE *f, uint16_t v) {
  uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
  if (fwrite(b, 1, 2, f) != 2) fail("Cannot write output");
}
static void wr32(FILE *f, uint32_t v) {
  uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  if (fwrite(b, 1, 4, f) != 4) fail("Cannot write output");
}
static void wrbytes(FILE *f, const void *p, size_t n) {
  if (n && fwrite(p, 1, n, f) != n) fail("Cannot write output");
}
static void wrbe32(std::string &s, uint32_t v) {
  s.push_back(char(v >> 24)); s.push_back(char(v >> 16));
  s.push_back(char(v >> 8)); s.push_back(char(v));
}

static std::vector<uint8_t> read_all(const std::string &path, size_t limit = 128u * 1024u * 1024u) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) fail("Cannot open file: " + path);
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fail("Cannot seek file"); }
  long n = ftell(f);
  if (n < 0) { fclose(f); fail("Cannot stat file"); }
  if (size_t(n) > limit) { fclose(f); fail("File exceeds import limit"); }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); fail("Cannot rewind file"); }
  std::vector<uint8_t> out(static_cast<size_t>(n));
  if (!out.empty() && fread(out.data(), 1, out.size(), f) != out.size()) {
    fclose(f); fail("Cannot read file");
  }
  fclose(f);
  return out;
}

static bool file_exists(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

static std::string dirname_of(const std::string &p) {
  size_t at = p.find_last_of('/');
  return at == std::string::npos ? "." : p.substr(0, at);
}

static void mkdir_recursive(const std::string &path) {
  if (path.empty() || path == "/") return;
  std::string cur;
  if (path[0] == '/') cur = "/";
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty() && cur != "/") mkdir(cur.c_str(), 0777);
    } else {
      cur.push_back(path[i]);
      continue;
    }
    if (i < path.size() && path[i] == '/') {
      if (cur.empty()) cur = path.substr(0, i + 1);
      else if (cur.back() != '/') cur.push_back('/');
    }
  }
  mkdir(path.c_str(), 0777);
}

static void remove_tree(const std::string &path) {
  DIR *d = opendir(path.c_str());
  if (!d) { remove(path.c_str()); return; }
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    std::string child = path + "/" + e->d_name;
    struct stat st{};
    if (stat(child.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) remove_tree(child);
    else remove(child.c_str());
  }
  closedir(d);
  rmdir(path.c_str());
}

struct Sha256 {
  uint32_t h[8] = {
    0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
    0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
  };
  uint64_t bits = 0;
  uint8_t buf[64]{};
  size_t used = 0;

  static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
  }

  void block(const uint8_t *p) {
    static const uint32_t k[64] = {
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
      0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
      0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
      0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
      0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
      0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
      0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(p[i*4]) << 24) | (uint32_t(p[i*4+1]) << 16) |
             (uint32_t(p[i*4+2]) << 8) | uint32_t(p[i*4+3]);
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
      uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],x=h[7];
    for (int i=0;i<64;++i) {
      uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
      uint32_t ch=(e&f)^((~e)&g);
      uint32_t t1=x+S1+ch+k[i]+w[i];
      uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
      uint32_t maj=(a&b)^(a&c)^(b&c);
      uint32_t t2=S0+maj;
      x=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=x;
  }

  void update(const void *vp, size_t n) {
    const uint8_t *p=static_cast<const uint8_t *>(vp);
    bits += uint64_t(n)*8;
    while(n) {
      size_t take=64-used;
      if(take>n)take=n;
      memcpy(buf+used,p,take);used+=take;p+=take;n-=take;
      if(used==64){block(buf);used=0;}
    }
  }

  std::string finish_hex() {
    uint8_t tail[128]{};
    size_t n=used;
    memcpy(tail,buf,n);tail[n++]=0x80;
    while((n%64)!=56)n++;
    uint64_t be=bits;
    for(int i=0;i<8;++i)tail[n+7-i]=uint8_t(be>>(i*8));
    n+=8;
    for(size_t i=0;i<n;i+=64)block(tail+i);
    static const char *hex="0123456789abcdef";
    std::string out;
    out.reserve(64);
    for(uint32_t v:h)for(int i=7;i>=0;--i)out.push_back(hex[(v>>(i*4))&15]);
    return out;
  }
};

static std::string sha256(const std::vector<uint8_t> &data) {
  Sha256 s;s.update(data.data(),data.size());return s.finish_hex();
}
static std::string sha256_file(const std::string &path) {
  FILE *f=fopen(path.c_str(),"rb");if(!f)fail("Cannot open JAR");
  Sha256 s;uint8_t b[65536];
  while(true){size_t n=fread(b,1,sizeof b,f);if(n)s.update(b,n);if(n<sizeof b){if(ferror(f)){fclose(f);fail("Cannot read JAR");}break;}}
  fclose(f);return s.finish_hex();
}

struct JsonObj;
struct JsonArr;
using Json = std::variant<std::monostate,bool,int64_t,double,std::string,
                          std::shared_ptr<JsonArr>,std::shared_ptr<JsonObj>>;
struct JsonArr { std::vector<Json> v; };
struct JsonObj { std::map<std::string,Json> v; };

static std::string jesc(const std::string &s) {
  std::string o="\"";
  static const char *hex="0123456789abcdef";
  for(unsigned char c:s){
    switch(c){
      case '\"':o+="\\\"";break; case '\\':o+="\\\\";break;
      case '\b':o+="\\b";break; case '\f':o+="\\f";break;
      case '\n':o+="\\n";break; case '\r':o+="\\r";break; case '\t':o+="\\t";break;
      default:
        if(c<0x20){o+="\\u00";o.push_back(hex[c>>4]);o.push_back(hex[c&15]);}
        else o.push_back(char(c));
    }
  }
  o.push_back('\"');return o;
}
static void jwrite(std::string &o,const Json &x);
static void jwrite(std::string &o,const std::shared_ptr<JsonArr> &a){
  o.push_back('[');for(size_t i=0;i<a->v.size();++i){if(i)o.push_back(',');jwrite(o,a->v[i]);}o.push_back(']');
}
static void jwrite(std::string &o,const std::shared_ptr<JsonObj> &a){
  o.push_back('{');size_t i=0;for(const auto &kv:a->v){if(i++)o.push_back(',');o+=jesc(kv.first);o.push_back(':');jwrite(o,kv.second);}o.push_back('}');
}
static void jwrite(std::string &o,const Json &x){
  if(std::holds_alternative<std::monostate>(x))o+="null";
  else if(auto p=std::get_if<bool>(&x))o+=*p?"true":"false";
  else if(auto p=std::get_if<int64_t>(&x))o+=std::to_string(*p);
  else if(auto p=std::get_if<double>(&x)){char b[64];snprintf(b,sizeof b,"%.17g",*p);o+=b;}
  else if(auto p=std::get_if<std::string>(&x))o+=jesc(*p);
  else if(auto p=std::get_if<std::shared_ptr<JsonArr>>(&x))jwrite(o,*p);
  else jwrite(o,std::get<std::shared_ptr<JsonObj>>(x));
}
static std::string json_string(const Json &x){std::string o;jwrite(o,x);return o;}

static std::shared_ptr<JsonObj> jobj(){return std::make_shared<JsonObj>();}
static std::shared_ptr<JsonArr> jarr(){return std::make_shared<JsonArr>();}
static Json ji(int64_t v){return v;}
static Json jd(double v){return v;}
static Json js(const std::string &v){return v;}
static Json jo(const std::shared_ptr<JsonObj> &v){return v;}
static Json ja(const std::shared_ptr<JsonArr> &v){return v;}

struct ZipEntry {
  std::string name;
  uint16_t flags=0,method=0;
  uint32_t csize=0,size=0,local=0,crc=0;
};

struct ZipReader {
  std::vector<uint8_t> raw;
  std::map<std::string,ZipEntry> entries;

  explicit ZipReader(const std::string &path):raw(read_all(path,16u*1024u*1024u)) {
    size_t start=raw.size()>65557?raw.size()-65557:0;size_t eocd=SIZE_MAX;
    for(size_t i=raw.size();i>=start+4;--i)if(rd32(raw.data()+i-4)==0x06054b50u){eocd=i-4;break;}
    if(eocd==SIZE_MAX)fail("Invalid JAR: missing ZIP end record");
    uint16_t count=rd16(raw.data()+eocd+10);
    uint32_t cdsize=rd32(raw.data()+eocd+12),cd=rd32(raw.data()+eocd+16);
    if(rd16(raw.data()+eocd+4)||rd16(raw.data()+eocd+6)||!count||count>4096u)fail("Unsupported ZIP layout");
    if(uint64_t(cd)+cdsize>raw.size()||uint64_t(cd)+cdsize!=eocd)fail("Invalid ZIP central directory");
    size_t p=cd;
    for(uint16_t i=0;i<count;++i){
      if(p+46>eocd||rd32(raw.data()+p)!=0x02014b50u)fail("Invalid JAR central directory");
      ZipEntry z;
      z.flags=rd16(raw.data()+p+8);z.method=rd16(raw.data()+p+10);z.crc=rd32(raw.data()+p+16);
      z.csize=rd32(raw.data()+p+20);z.size=rd32(raw.data()+p+24);
      uint16_t nl=rd16(raw.data()+p+28),el=rd16(raw.data()+p+30),cl=rd16(raw.data()+p+32);
      z.local=rd32(raw.data()+p+42);
      if(p+46+nl+el+cl>eocd)fail("Invalid JAR entry");
      z.name.assign(reinterpret_cast<const char *>(raw.data()+p+46),nl);
      if(entries.count(z.name))fail("Duplicate JAR entry");
      if((z.flags&1)||z.method>8||z.size>16u*1024u*1024u)fail("Unsupported JAR entry");
      entries[z.name]=z;p+=46+nl+el+cl;
    }
  }

  bool has(const std::string &name)const{return entries.find(name)!=entries.end();}

  std::vector<uint8_t> read(const std::string &name)const {
    auto it=entries.find(name);if(it==entries.end())fail("Missing JAR entry: "+name);
    const ZipEntry &z=it->second;size_t p=z.local;
    if(p+30>raw.size()||rd32(raw.data()+p)!=0x04034b50u)fail("Invalid JAR local header");
    uint16_t nl=rd16(raw.data()+p+26),el=rd16(raw.data()+p+28);
    size_t data=p+30+nl+el;
    if(uint64_t(data)+z.csize>raw.size())fail("Truncated JAR entry");
    std::vector<uint8_t> out(z.size);
    if(z.method==0){if(z.csize!=z.size)fail("Invalid stored JAR entry");memcpy(out.data(),raw.data()+data,z.size);}
    else{
      z_stream s{};s.next_in=const_cast<Bytef *>(raw.data()+data);s.avail_in=z.csize;s.next_out=out.data();s.avail_out=z.size;
      if(inflateInit2(&s,-MAX_WBITS)!=Z_OK)fail("Cannot initialize ZIP inflater");
      int r=inflate(&s,Z_FINISH);inflateEnd(&s);
      if(r!=Z_STREAM_END||s.total_out!=z.size)fail("Invalid compressed JAR entry");
    }
    if(crc32(0,out.data(),out.size())!=z.crc)fail("JAR entry CRC mismatch");
    return out;
  }

  std::vector<std::string> names()const{
    std::vector<std::string> r;for(auto &x:entries)r.push_back(x.first);return r;
  }
};

struct PackItem { std::string name;std::vector<uint8_t> data;uint32_t crc=0;uint32_t offset=0; };

static void write_zip_stored(const std::string &path, std::vector<PackItem> items) {
  mkdir_recursive(dirname_of(path));
  FILE *f=fopen(path.c_str(),"wb");if(!f)fail("Cannot create content pack");
  uint32_t offset=0;
  for(auto &it:items){
    it.offset=offset;it.crc=crc32(0,it.data.data(),it.data.size());
    wr32(f,0x04034b50u);wr16(f,20);wr16(f,0);wr16(f,0);wr16(f,0);wr16(f,0);
    wr32(f,it.crc);wr32(f,uint32_t(it.data.size()));wr32(f,uint32_t(it.data.size()));
    wr16(f,uint16_t(it.name.size()));wr16(f,0);wrbytes(f,it.name.data(),it.name.size());wrbytes(f,it.data.data(),it.data.size());
    offset += 30u+uint32_t(it.name.size())+uint32_t(it.data.size());
  }
  uint32_t cd_offset=offset;
  for(const auto &it:items){
    wr32(f,0x02014b50u);wr16(f,20);wr16(f,20);wr16(f,0);wr16(f,0);wr16(f,0);wr16(f,0);
    wr32(f,it.crc);wr32(f,uint32_t(it.data.size()));wr32(f,uint32_t(it.data.size()));
    wr16(f,uint16_t(it.name.size()));wr16(f,0);wr16(f,0);wr16(f,0);wr16(f,0);wr32(f,0);wr32(f,it.offset);
    wrbytes(f,it.name.data(),it.name.size());offset+=46u+uint32_t(it.name.size());
  }
  uint32_t cd_size=offset-cd_offset;
  wr32(f,0x06054b50u);wr16(f,0);wr16(f,0);wr16(f,uint16_t(items.size()));wr16(f,uint16_t(items.size()));wr32(f,cd_size);wr32(f,cd_offset);wr16(f,0);
  fclose(f);
}

static std::string mutf8(const std::vector<uint8_t> &raw) {
  std::vector<uint16_t> u;
  for(size_t i=0;i<raw.size();){
    uint8_t c=raw[i++];
    if(c==0xC0 && i<raw.size() && raw[i]==0x80){++i;u.push_back(0);continue;}
    if(c<0x80){u.push_back(c);continue;}
    if((c&0xE0)==0xC0 && i<raw.size()){u.push_back(uint16_t((c&31)<<6)|(raw[i++]&63));continue;}
    if((c&0xF0)==0xE0 && i+1<raw.size()){
      uint8_t c1=raw[i++],c2=raw[i++];
      u.push_back(uint16_t((c&15)<<12)|((c1&63)<<6)|(c2&63));continue;
    }
    fail("Invalid modified UTF-8");
  }
  std::string out;
  for(size_t i=0;i<u.size();++i){
    uint32_t cp=u[i];
    if(cp>=0xD800&&cp<=0xDBFF&&i+1<u.size()&&u[i+1]>=0xDC00&&u[i+1]<=0xDFFF){
      cp=0x10000+((cp-0xD800)<<10)+(u[++i]-0xDC00);
    }
    if(cp<0x80)out.push_back(char(cp));
    else if(cp<0x800){out.push_back(char(0xC0|(cp>>6)));out.push_back(char(0x80|(cp&63)));}
    else if(cp<0x10000){out.push_back(char(0xE0|(cp>>12)));out.push_back(char(0x80|((cp>>6)&63)));out.push_back(char(0x80|(cp&63)));}
    else {out.push_back(char(0xF0|(cp>>18)));out.push_back(char(0x80|((cp>>12)&63)));out.push_back(char(0x80|((cp>>6)&63)));out.push_back(char(0x80|(cp&63)));}
  }
  return out;
}

struct Rdr {
  const std::vector<uint8_t> &d;size_t p=0;
  explicit Rdr(const std::vector<uint8_t> &x):d(x){}
  uint8_t u1(){if(p+1>d.size())fail("Truncated class data");return d[p++];}
  uint16_t u2(){if(p+2>d.size())fail("Truncated class data");uint16_t v=be16(d.data()+p);p+=2;return v;}
  uint32_t u4(){if(p+4>d.size())fail("Truncated class data");uint32_t v=be32(d.data()+p);p+=4;return v;}
  int8_t s1(){return int8_t(u1());}
  int16_t s2(){return int16_t(u2());}
  int32_t s4(){return int32_t(u4());}
  std::vector<uint8_t> take(size_t n){if(p+n>d.size())fail("Truncated class data");std::vector<uint8_t> v(d.begin()+p,d.begin()+p+n);p+=n;return v;}
};

struct CP { int tag=0; std::variant<std::monostate,int64_t,double,std::string,uint16_t,std::pair<uint16_t,uint16_t>> v; };

struct Member {uint16_t flags=0;std::map<std::string,std::vector<uint8_t>> attrs;};

struct ClassData {
  std::vector<CP> pool;std::map<std::pair<std::string,std::string>,Member> fields,methods;std::string name;

  explicit ClassData(const std::vector<uint8_t> &raw){
    Rdr r(raw);if(r.u4()!=0xcafebabeu)fail("Invalid class magic");r.u2();r.u2();uint16_t n=r.u2();pool.resize(n);size_t i=1;
    while(i<n){int tag=r.u1();pool[i].tag=tag;
      switch(tag){
        case 1:{auto x=r.take(r.u2());pool[i].v=mutf8(x);break;}
        case 3:pool[i].v=int64_t(int32_t(r.u4()));break;
        case 4:{uint32_t b=r.u4();float f;memcpy(&f,&b,4);pool[i].v=double(f);break;}
        case 5:{uint64_t hi=r.u4(),lo=r.u4();pool[i].v=int64_t((hi<<32)|lo);break;}
        case 6:{uint64_t hi=r.u4(),lo=r.u4(),b=(hi<<32)|lo;double d;memcpy(&d,&b,8);pool[i].v=d;break;}
        case 7:case 8:case 16:case 19:case 20:pool[i].v=r.u2();break;
        case 9:case 10:case 11:case 12:case 17:case 18:pool[i].v=std::make_pair(r.u2(),r.u2());break;
        case 15:{r.u1();pool[i].v=r.u2();break;}
        default:fail("Unsupported constant tag");
      }
      i+=(tag==5||tag==6)?2:1;
    }
    r.u2();name=constant_str(r.u2());uint16_t interfaces=r.u2();for(uint16_t j=0;j<interfaces;++j)r.u2();
    fields=members(r);methods=members(r);auto a=attrs(r);if(r.p!=raw.size())fail("Trailing class data");(void)a;
  }

  std::map<std::string,std::vector<uint8_t>> attrs(Rdr &r){
    std::map<std::string,std::vector<uint8_t>> out;uint16_t n=r.u2();
    for(uint16_t i=0;i<n;++i){std::string name=constant_str(r.u2());out[name]=r.take(r.u4());}
    return out;
  }
  std::map<std::pair<std::string,std::string>,Member> members(Rdr &r){
    std::map<std::pair<std::string,std::string>,Member> out;uint16_t n=r.u2();
    for(uint16_t i=0;i<n;++i){Member m;m.flags=r.u2();std::string name=constant_str(r.u2()),desc=constant_str(r.u2());m.attrs=attrs(r);out[{name,desc}]=std::move(m);}
    return out;
  }
  std::string constant_str(uint16_t idx)const{
    if(idx==0||idx>=pool.size())fail("Bad constant index");
    const CP &c=pool[idx];
    if(c.tag==1)return std::get<std::string>(c.v);
    if(c.tag==7||c.tag==8||c.tag==16||c.tag==19||c.tag==20)return constant_str(std::get<uint16_t>(c.v));
    fail("Constant is not a string");return "";
  }
  CP constant(uint16_t idx)const{
    if(idx==0||idx>=pool.size())fail("Bad constant index");
    return pool[idx];
  }
  std::tuple<std::string,std::string,std::string> reference(uint16_t idx)const{
    auto p=std::get<std::pair<uint16_t,uint16_t>>(constant(idx).v);
    auto owner=constant_str(std::get<uint16_t>(constant(p.first).v));
    auto q=std::get<std::pair<uint16_t,uint16_t>>(constant(p.second).v);
    return {owner,constant_str(q.first),constant_str(q.second)};
  }
  std::pair<std::vector<uint8_t>,uint16_t> code(const std::string &name,const std::string &desc)const{
    auto it=methods.find({name,desc});if(it==methods.end())fail("Missing method "+name+desc);
    auto ai=it->second.attrs.find("Code");if(ai==it->second.attrs.end())fail("Method has no code "+name+desc);
    Rdr r(ai->second);r.u2();uint16_t locals=r.u2();uint32_t n=r.u4();return {r.take(n),locals};
  }
};

struct Obj;struct Arr;
using Val=std::variant<std::monostate,bool,int64_t,double,std::string,
                       std::shared_ptr<Obj>,std::shared_ptr<Arr>>;
struct Obj {std::string type;std::map<std::string,Val> f;};
struct Arr {std::vector<Val> v;};
static std::string val_string(const Val&v){
  if(auto p=std::get_if<std::string>(&v))return *p;
  if(auto p=std::get_if<int64_t>(&v))return std::to_string(*p);
  if(auto p=std::get_if<bool>(&v))return *p?"True":"False";
  if(auto p=std::get_if<double>(&v)){char b[64];snprintf(b,sizeof b,"%.17g",*p);return b;}
  return "";
}

static bool is_null(const Val &v){return std::holds_alternative<std::monostate>(v);}
static bool same_ref(const Val&a,const Val&b){
  if(std::holds_alternative<std::monostate>(a)||std::holds_alternative<std::monostate>(b))
    return std::holds_alternative<std::monostate>(a)&&std::holds_alternative<std::monostate>(b);
  if(auto x=std::get_if<std::shared_ptr<Obj>>(&a))return std::holds_alternative<std::shared_ptr<Obj>>(b)&&*x==std::get<std::shared_ptr<Obj>>(b);
  if(auto x=std::get_if<std::shared_ptr<Arr>>(&a))return std::holds_alternative<std::shared_ptr<Arr>>(b)&&*x==std::get<std::shared_ptr<Arr>>(b);
  return false;
}
static int64_t as_i(const Val &v){if(auto p=std::get_if<int64_t>(&v))return *p;if(auto p=std::get_if<bool>(&v))return *p?1:0;if(auto p=std::get_if<double>(&v))return int64_t(*p);fail("Expected number");return 0;}
static double as_d(const Val &v){if(auto p=std::get_if<double>(&v))return *p;if(auto p=std::get_if<int64_t>(&v))return double(*p);fail("Expected number");return 0;}
static bool veq(const Val &a,const Val &b){
  if(a.index()!=b.index()){
    if((std::holds_alternative<int64_t>(a)||std::holds_alternative<double>(a))&&
       (std::holds_alternative<int64_t>(b)||std::holds_alternative<double>(b)))return as_d(a)==as_d(b);
    return false;
  }
  if(std::holds_alternative<std::monostate>(a))return true;
  if(auto p=std::get_if<bool>(&a))return *p==std::get<bool>(b);
  if(auto p=std::get_if<int64_t>(&a))return *p==std::get<int64_t>(b);
  if(auto p=std::get_if<double>(&a))return *p==std::get<double>(b);
  if(auto p=std::get_if<std::string>(&a))return *p==std::get<std::string>(b);
  if(auto p=std::get_if<std::shared_ptr<Obj>>(&a))return p->get()==std::get<std::shared_ptr<Obj>>(b).get();
  return std::get<std::shared_ptr<Arr>>(a).get()==std::get<std::shared_ptr<Arr>>(b).get();
}
static std::string desc_type(const std::string &d){
  static const std::map<std::string,std::string> t={{"I","int"},{"S","short"},{"B","byte"},{"Z","boolean"},{"J","long"},{"F","float"},{"D","double"},{"C","char"},{"Ljava/lang/String;","java.lang.String"}};
  auto it=t.find(d);return it!=t.end()?it->second:d;
}
static Val defval(const std::string &d){
  return (!d.empty()&&(d[0]=='L'||d[0]=='['))?Val(std::monostate{}):Val(int64_t(0));
}
static int argc_desc(const std::string &d){
  if(d.size()<2)return 0;
  size_t p=1,n=0;
  while(p<d.size()&&d[p]!=')'){
    if(d[p]=='['){
      while(p<d.size()&&d[p]=='[')++p;
      if(p<d.size()&&d[p]=='L')while(p<d.size()&&d[p++]!=';');
      else if(p<d.size())++p;
    } else if(d[p]=='L') {
      while(p<d.size()&&d[p++]!=';');
    } else {
      ++p;
    }
    ++n;
  }
  return int(n);
}

using StaticMap=std::map<std::tuple<std::string,std::string,std::string>,Val>;

struct Evaluator {
  const std::map<std::string,ClassData> &classes;
  StaticMap &statics;
  std::function<Val(const std::string&,const std::string&,const std::string&,const std::shared_ptr<Obj>&,const std::vector<Val>&)> summary;

  Val constant_value(const ClassData &c,uint16_t idx){
    CP x=c.constant(idx);
    switch(x.tag){
      case 1:case 7:case 8:case 16:case 19:case 20:return c.constant_str(idx);
      case 3:case 5:return std::get<int64_t>(x.v);
      case 4:case 6:return std::get<double>(x.v);
      default:fail("Unsupported constant value");
    }
    return {};
  }

  static int32_t i32(const std::vector<uint8_t> &c,size_t &p){if(p+4>c.size())fail("Invalid branch");int32_t v=int32_t((uint32_t(c[p])<<24)|(uint32_t(c[p+1])<<16)|(uint32_t(c[p+2])<<8)|c[p+3]);p+=4;return v;}
  static int16_t i16(const std::vector<uint8_t> &c,size_t &p){if(p+2>c.size())fail("Invalid branch");int16_t v=int16_t((uint16_t(c[p])<<8)|c[p+1]);p+=2;return v;}

  Val run(const std::string &cn,const std::string &mn,const std::string &md,const std::vector<Val> &args){
    auto ci=classes.find(cn);if(ci==classes.end())fail("Missing class "+cn);const ClassData &cl=ci->second;
    auto cd=cl.code(mn,md);const auto &code=cd.first;size_t locals_n=cd.second;
    std::vector<Val> local(locals_n);for(size_t i=0;i<args.size()&&i<local.size();++i)local[i]=args[i];
    std::vector<Val> st;size_t p=0,branch_base=0;
    auto branch=[&](int16_t off){int64_t t=int64_t(branch_base)+off;if(t<0||t>=int64_t(code.size()))fail("Invalid branch");p=size_t(t);};
    auto bit_reader=[&](std::shared_ptr<Arr> a,size_t idx)->Val{if(!a||idx>=a->v.size())fail("Invalid array access");return a->v[idx];};
    for(int step=0;step<100000;++step){
      if(p>=code.size())fail("Method fell off end");
      size_t start=p;branch_base=start;uint8_t op=code[p++];
      switch(op){
        case 0:break;
        case 1:st.emplace_back(std::monostate{});break;
        case 2:case 3:case 4:case 5:case 6:case 7:case 8:st.emplace_back(int64_t(op-3));break;
        case 9:case 10:case 11:case 12:case 13:case 14:case 15:{double a=(op==9?0:op==10?1:op==11?0:op==12?1:op==13?2:op==14?0:1);if(op<=10)st.emplace_back(int64_t(a));else st.emplace_back(a);break;}
        case 16:if(p+1>code.size())fail("Invalid bipush");st.emplace_back(int64_t(int8_t(code[p++])));break;
        case 17:st.emplace_back(int64_t(i16(code,p)));break;
        case 18:st.push_back(constant_value(cl,code[p++]));break;
        case 19:case 20:{if(p+2>code.size())fail("Invalid ldc_w");uint16_t idx=rd16(code.data()+p);p+=2;st.push_back(constant_value(cl,idx));break;}
        case 21:case 22:case 23:case 24:case 25:{uint8_t i=code[p++];if(i>=local.size())fail("Invalid local");st.push_back(local[i]);break;}
        case 26:case 27:case 28:case 29:case 30:case 31:case 32:case 33:case 34:case 35:
        case 36:case 37:case 38:case 39:case 40:case 41:case 42:case 43:case 44:case 45:{size_t i=(op-26)%4;if(i>=local.size())fail("Invalid local");st.push_back(local[i]);break;}
        case 46:case 47:case 48:case 49:case 50:case 51:case 52:case 53:{int64_t idx=as_i(st.back());st.pop_back();auto a=std::get<std::shared_ptr<Arr>>(st.back());st.pop_back();st.push_back(bit_reader(a,size_t(idx)));break;}
        case 54:case 55:case 56:case 57:case 58:{uint8_t i=code[p++];if(i>=local.size())fail("Invalid local");local[i]=st.back();st.pop_back();break;}
        case 59:case 60:case 61:case 62:case 63:case 64:case 65:case 66:case 67:case 68:case 69:case 70:case 71:case 72:case 73:case 74:case 75:case 76:case 77:case 78:{size_t i=(op-59)%4;if(i>=local.size())fail("Invalid local");local[i]=st.back();st.pop_back();break;}
        case 79:case 80:case 81:case 82:case 83:case 84:case 85:case 86:{Val v=st.back();st.pop_back();size_t idx=size_t(as_i(st.back()));st.pop_back();auto a=std::get<std::shared_ptr<Arr>>(st.back());st.pop_back();if(!a||idx>=a->v.size())fail("Invalid literal array index");a->v[idx]=v;break;}
        case 87:st.pop_back();break;
        case 89:st.push_back(st.back());break;
        case 90:{Val a=st.back();st.pop_back();Val b=st.back();st.pop_back();st.push_back(a);st.push_back(b);st.push_back(a);break;}
        case 92:{Val a=st[st.size()-2],b=st.back();st.push_back(a);st.push_back(b);break;}
        case 95:std::swap(st[st.size()-1],st[st.size()-2]);break;
        case 96:case 100:case 104:case 108:case 112:case 120:case 122:case 124:case 126:case 128:case 130:{
          int64_t b=as_i(st.back());st.pop_back();int64_t a=as_i(st.back());st.pop_back();int64_t v=0;
          if(op==96)v=a+b;else if(op==100)v=a-b;else if(op==104)v=a*b;else if(op==108){if(!b)fail("Division by zero");v=int64_t(a/b);}
          else if(op==112){if(!b)fail("Division by zero");v=a-int64_t(a/b)*b;}else if(op==120)v=a<<(b&31);else if(op==122)v=a>>(b&31);
          else if(op==124)v=int64_t(uint64_t(uint32_t(a))>>(b&31));else if(op==126)v=a&b;else if(op==128)v=a|b;else v=a^b;
          v=int64_t(int32_t(uint32_t(v)));st.emplace_back(v);break;
        }
        case 116:st.back()=Val(-as_i(st.back()));break;
        case 132:{uint8_t i=code[p++];int8_t c=int8_t(code[p++]);local[i]=Val(as_i(local[i])+c);break;}
        case 133:case 134:case 135:case 136:case 139:case 142:{Val a=st.back();st.pop_back();if(op==134||op==135)st.emplace_back(as_d(a));else st.emplace_back(as_i(a));break;}
        case 145:{int64_t a=as_i(st.back());st.back()=Val((a+128)%256-128);break;}
        case 146:{int64_t a=as_i(st.back());st.back()=Val((a%65536+65536)%65536);break;}
        case 147:{int64_t a=as_i(st.back());int64_t v=(a+32768)%65536-32768;st.back()=Val(v);break;}
        case 153:case 154:case 155:case 156:case 157:case 158:case 159:case 160:case 161:case 162:case 163:case 164:{
          int16_t off=i16(code,p);Val b;if(op>=159)b=st.back(),st.pop_back();Val a=st.back();st.pop_back();bool c=false;int k=(op-153)%6;
          if(op<=158){int64_t av=as_i(a);c=(k==0?av==0:k==1?av!=0:k==2?av<0:k==3?av>=0:k==4?av>0:av<=0);}
          else {int64_t av=as_i(a),bv=as_i(b);c=(k==0?av==bv:k==1?av!=bv:k==2?av<bv:k==3?av>=bv:k==4?av>bv:av<=bv);}
          if(c)branch(off);
          break;
        }
        case 165:case 166:{int16_t off=i16(code,p);Val b=st.back();st.pop_back();Val a=st.back();st.pop_back();if(same_ref(a,b)==(op==165))branch(off);break;}
        case 167:branch(i16(code,p));break;
        case 170:case 171:{
          while(p%4)p++;
          int32_t fallback=i32(code,p);int64_t key=as_i(st.back());st.pop_back();int32_t chosen=fallback;
          if(op==170){int32_t low=i32(code,p),high=i32(code,p);if(high-low>10000)fail("Oversized switch");for(int64_t k=low;k<=high;++k){int32_t off=i32(code,p);if(k==key)chosen=off;}}
          else {int32_t count=i32(code,p);if(count<0||count>10000)fail("Oversized switch");for(int32_t j=0;j<count;++j){int32_t k=i32(code,p),off=i32(code,p);if(k==key)chosen=off;}}
          int64_t t=int64_t(start)+chosen;if(t<0||t>=int64_t(code.size()))fail("Invalid switch");p=size_t(t);break;
        }
        case 172:case 173:case 174:case 175:case 176:{Val r=st.back();return r;}
        case 177:return {};
        case 178:case 179:case 180:case 181:{
          uint16_t idx=rd16(code.data()+p);p+=2;auto [owner,field,fd]=cl.reference(idx);auto key=std::make_tuple(owner,field,fd);
          if(op==178){auto it=statics.find(key);if(it==statics.end())fail("Unspecified static input");st.push_back(it->second);}
          else if(op==179){statics[key]=st.back();st.pop_back();}
          else if(op==180){auto o=std::get<std::shared_ptr<Obj>>(st.back());st.pop_back();st.push_back(o&&o->f.count(field+":"+fd)?o->f[field+":"+fd]:defval(fd));}
        else {Val v=st.back();st.pop_back();auto o=std::get<std::shared_ptr<Obj>>(st.back());st.pop_back();if(o)o->f[field+":"+fd]=v;}
          break;
        }
        case 182:case 183:case 184:{
          uint16_t idx=rd16(code.data()+p);p+=2;auto [owner,name,desc]=cl.reference(idx);int n=argc_desc(desc);std::vector<Val> aa(n);for(int j=n-1;j>=0;--j){aa[j]=st.back();st.pop_back();}
          std::shared_ptr<Obj> obj;if(op!=184){obj=std::get<std::shared_ptr<Obj>>(st.back());st.pop_back();}
          Val ret=summary(owner,name,desc,obj,aa);if(!desc.empty()&&desc.back()!='V')st.push_back(ret);break;
        }
        case 187:{uint16_t idx=rd16(code.data()+p);p+=2;auto o=std::make_shared<Obj>();o->type=cl.constant_str(idx);st.push_back(o);break;}
        case 188:{uint8_t type=code[p++];(void)type;int64_t n=as_i(st.back());st.pop_back();if(n<0||n>100000)fail("Oversized literal array");auto a=std::make_shared<Arr>();a->v.resize(size_t(n),Val(int64_t(0)));st.push_back(a);break;}
        case 189:{p+=2;int64_t n=as_i(st.back());st.pop_back();if(n<0||n>100000)fail("Oversized literal array");auto a=std::make_shared<Arr>();a->v.resize(size_t(n),Val(std::monostate{}));st.push_back(a);break;}
        case 190:{auto a=std::get<std::shared_ptr<Arr>>(st.back());st.back()=Val(int64_t(a?a->v.size():0));break;}
        case 192:p+=2;break;
        case 198:case 199:{int16_t off=i16(code,p);Val a=st.back();st.pop_back();bool c=is_null(a)==(op==198);if(c)branch(off);break;}
        default:fail("Unsupported data opcode");
      }
      (void)start;
    }
    fail("Data evaluation instruction budget exceeded");return {};
  }
};

static Json val_json(const Val &v) {
  if (std::holds_alternative<std::monostate>(v)) return Json(std::monostate{});
  if (auto p=std::get_if<bool>(&v)) return *p;
  if (auto p=std::get_if<int64_t>(&v)) return ji(*p);
  if (auto p=std::get_if<double>(&v)) return jd(*p);
  if (auto p=std::get_if<std::string>(&v)) return js(*p);
  if (auto p=std::get_if<std::shared_ptr<Arr>>(&v)) {
    auto a=jarr(); if (*p) for (const auto &x:(*p)->v) a->v.push_back(val_json(x)); return ja(a);
  }
  auto o=jobj();
  if (auto p=std::get_if<std::shared_ptr<Obj>>(&v); p && *p) {
    o->v["_type"]=js((*p)->type);
    for (const auto &kv:(*p)->f) {
      if (kv.first.find(':')==std::string::npos) continue;
      size_t q=kv.first.find(':');std::string n=kv.first.substr(0,q),d=kv.first.substr(q+1);
      o->v[n+":"+desc_type(d)]=val_json(kv.second);
    }
  }
  return jo(o);
}

static Json record_json(const std::shared_ptr<Obj> &obj) {
  auto out=jobj(); if (!obj) return jo(out);
  for (const auto &kv:obj->f) {
    if (kv.first.find(':')==std::string::npos) continue;
    size_t q=kv.first.find(':');std::string n=kv.first.substr(0,q),d=kv.first.substr(q+1);
    out->v[n+":"+desc_type(d)]=val_json(kv.second);
  }
  return jo(out);
}

struct Bits {
  const std::vector<uint8_t> &d;size_t p=0;uint32_t cache=0;int cached=0;
  explicit Bits(const std::vector<uint8_t> &x):d(x){}
  uint8_t u8(){if(p>=d.size())fail("Truncated Micro3D resource");return d[p++];}
  uint16_t u16(){if(p+2>d.size())fail("Truncated Micro3D resource");uint16_t v=rd16(d.data()+p);p+=2;return v;}
  int16_t s16(){return int16_t(u16());}
  int32_t i32(){if(p+4>d.size())fail("Truncated Micro3D resource");int32_t v=int32_t(rd32(d.data()+p));p+=4;return v;}
  void skip(size_t n){if(p+n>d.size())fail("Truncated Micro3D resource");p+=n;}
  uint32_t bits(int n,bool sign=false){if(n<0||n>32)fail("Invalid bit width");while(cached<n){cache|=uint32_t(u8())<<cached;cached+=8;}uint32_t v=cache&((n==32)?0xffffffffu:((1u<<n)-1));cache>>=n;cached-=n;return sign&&n&&(v&(1u<<(n-1)))?v-(1u<<n):v;}
  void align(){cache=0;cached=0;}
  std::vector<double> matrix(){std::vector<double> m(12);for(int i=0;i<12;++i)m[i]=double(s16())*(i%4==3?1.0:1.0/4096.0);return m;}
  int header(const char *magic){if(u8()!=uint8_t(magic[0])||u8()!=uint8_t(magic[1]))fail("Invalid Micro3D signature");int v=u8();if(u8()||v<2||v>5)fail("Unsupported Micro3D version");return v;}
};

static Json mesh_vec(const std::vector<double> &v){auto a=jarr();for(double x:v)a->v.push_back(jd(x));return ja(a);}
static Json mesh_vec_i(const std::vector<int64_t> &v){auto a=jarr();for(auto x:v)a->v.push_back(ji(x));return ja(a);}

struct DecodedModel { std::shared_ptr<JsonObj> json; std::map<std::string,std::vector<double>> geometry; };

static DecodedModel micro_model(const std::vector<uint8_t> &data){
  Bits r(data);int version=r.header("MB");
  int vf,nf,pf,bf;if(version>3){vf=r.u8();nf=r.u8();pf=r.u8();bf=r.u8();}else{vf=1;nf=0;pf=1;bf=1;}
  int nv=r.u16(),t3=r.u16(),t4=r.u16(),nb=r.u16();int c3=0,c4=0,nt=1,np=1,nc=0;
  if(pf>=3){c3=r.u16();c4=r.u16();nt=r.u16();np=r.u16();nc=r.u16();}else{c3=0;c4=0;nt=1;np=1;nc=0;}
  if(bf!=1||nv>21845||nb>nv||nt>16||np<1||np>33||nc>256)fail("Invalid model dimensions");
  std::vector<std::vector<std::array<int,2>>> patterns;
  if(version==5){patterns.resize(np,std::vector<std::array<int,2>>(nt+1));for(int i=0;i<np;++i)for(int j=0;j<nt+1;++j)patterns[i][j]={int(r.u16()),int(r.u16())};}
  else patterns={{{c3,c4},{t3,t4}}};
  std::vector<int64_t> vertices;if(vf==1){vertices.resize(size_t(nv)*3);for(auto &x:vertices)x=r.s16();}
  else if(vf==2){
    static const int widths[]={8,10,13,16};
    while(vertices.size()<size_t(nv)*3){
      int ch=r.bits(8);int count=((ch&63)+1)*3;
      if(vertices.size()+size_t(count)>size_t(nv)*3)fail("Oversized vertex block");
      int width=widths[(ch>>6)&3];
      for(int i=0;i<count;++i)vertices.push_back(int32_t(r.bits(width,true)));
    }
  }
  else fail("Unsupported vertex encoding");
  r.align();std::vector<int64_t> normals;if(nf==1){normals.resize(size_t(nv)*3);for(auto &x:normals)x=r.s16();}
  else if(nf==2){for(int i=0;i<nv;++i){int x=r.bits(7);if(x==64){int kind=r.bits(3);if(kind>5)fail("Invalid normal");static const int normal_axis[]={0,0,64,0,0,-64,0,0};
int z=normal_axis[kind],y=normal_axis[kind+1],xx=normal_axis[kind+2];
normals.insert(normals.end(),{xx,y,z});}else{x=(x&64)?x-128:x;int y=r.bits(7,true);int sign=r.bits(1);int z=int(std::floor(std::sqrt(double(std::max(0,4096-x*x-y*y)))+.5))*(sign?-1:1);normals.insert(normals.end(),{x,y,z});}}}
  else if(nf!=0)fail("Unsupported normal encoding");
  r.align();
  struct Poly{std::vector<int> indices;std::vector<int> attr;int texture=-1;int pattern=0;int blend=0;bool double_sided=false;};
  auto polygon=[&](const std::vector<int>&ind,const std::vector<int>&attr,int material,int face){
    for(int i:ind)if(i<0||i>=nv)fail("Vertex index outside model");
    Poly p;p.texture=face;p.blend=material&6;p.double_sided=bool(material&16);
    if(ind.size()==3){p.indices=ind;for(int v:attr)p.attr.push_back(v&255);}
    else {int order[]={0,1,2,2,1,3};for(int q:order)p.indices.push_back(ind[q]);for(int q:order)for(int k=0;k<5;++k)p.attr.push_back(attr[q*5+k]&255);}
    return p;
  };
  std::vector<Poly> colored,textured;
  if(c3+c4){int mb=r.u8(),ib=r.u8(),cb=r.u8(),ci=r.u8();(void)ci;r.u8();std::vector<std::vector<int>> pal(nc,std::vector<int>(3));for(auto &q:pal)for(int &x:q)x=r.bits(cb);
    for(int i=0;i<c3+c4;++i){
      int m=r.bits(mb)<<1;if(m&0xFC09)fail("Invalid colored material");
      int cnt=i<c3?3:4;std::vector<int> ind(cnt);for(int &x:ind)x=r.bits(ib);
      int color=r.bits(ci);if(color>=nc)fail("Invalid palette index");
      std::vector<int> attr;
      for(int j=0;j<cnt;++j){
        attr.insert(attr.end(),pal[color].begin(),pal[color].end());
        attr.push_back((m&32)>>5);attr.push_back((m&64)>>6);
      }
      Poly p=polygon(ind,attr,m,-1);p.attr=attr;colored.push_back(std::move(p));
    }}
  if(t3+t4){int mb=0,ib=0,uv=0;if(pf==2){mb=r.u8();ib=r.u8();uv=7;}else if(pf==3){mb=r.bits(8);ib=r.bits(8);uv=r.bits(8);r.bits(8);}else if(pf!=1)fail("Unsupported polygon encoding");
    for(int i=0;i<t3+t4;++i){int cnt=i<t3?3:4,m=0;std::vector<int>ind(cnt),attr; if(pf==1){m=r.u16();if(m&(cnt==3?0xFFF9:0xFFF8)||(cnt==4&&!((m)&1)))fail("Invalid material");for(int &x:ind)x=r.u16();m=((m&4)<<2)|((m&2)>>1);for(int j=0;j<cnt;++j){attr.insert(attr.end(),{int(r.u8()),int(r.u8()),1,0,m&1});}}
      else{m=r.bits(mb);if(m&(pf==2?0xFF88:0xFC08))fail("Invalid material");for(int &x:ind)x=r.bits(ib);for(int j=0;j<cnt;++j)attr.insert(attr.end(),{int(r.bits(uv)),int(r.bits(uv)),(m&32)>>5,(m&64)>>6,m&1});}
      Poly p=polygon(ind,attr,m,-1);p.attr=attr;textured.push_back(std::move(p));
    }}
  r.align();std::array<int,4> cursor={0,c3,0,t3};for(size_t pi=0;pi<patterns.size();++pi){int32_t pat=pi==0?0:int32_t(uint32_t(1u<<(pi%32)));for(int face=0;face<2;++face)for(int kind=0;kind<2;++kind){int count=patterns[pi][face][kind],slot=kind+(face?2:0);for(int q=0;q<count;++q){std::vector<Poly>*arr=face?&textured:&colored;if(cursor[slot]>=int(arr->size()))fail("Invalid pattern counts");(*arr)[cursor[slot]].pattern=pat;if(face)(*arr)[cursor[slot]].texture=face-1;cursor[slot]++;}}}
  struct Bone{int vertices=0,parent=-1;std::vector<double> matrix;};std::vector<Bone>bones;for(int i=0;i<nb;++i){Bone b;b.vertices=r.u16();b.parent=r.s16();if(b.parent<-1||b.parent>=i)fail("Invalid bone parent");b.matrix=r.matrix();bones.push_back(std::move(b));}int sum=0;for(auto &b:bones)sum+=b.vertices;if(sum!=nv)fail("Invalid bone vertex blocks");
  std::vector<double> geometry;
  if(nv>0){
    std::vector<std::vector<double>> world;world.reserve(bones.size());
    for(size_t bi=0;bi<bones.size();++bi){
      const auto &b=bones[bi];std::vector<double> m=b.matrix;
      if(b.parent>=0){
        const auto &a=world[size_t(b.parent)];m.assign(12,0);
        for(int row=0;row<3;++row)for(int col=0;col<4;++col){
          double v=(col==3)?a[row*4+3]:0;
          for(int k=0;k<3;++k)v+=a[row*4+k]*b.matrix[k*4+col];
          m[row*4+col]=v;
        }
      }
      world.push_back(std::move(m));
    }
    std::array<double,3> low={INFINITY,INFINITY,INFINITY},high={-INFINITY,-INFINITY,-INFINITY};
    size_t cursor=0;
    for(size_t bi=0;bi<bones.size();++bi){
      const auto &b=bones[bi];const auto &m=world[bi];
      for(int n=0;n<b.vertices;++n){
        if(cursor+2>=vertices.size())fail("Invalid bone vertex range");
        double x=double(vertices[cursor++]),y=double(vertices[cursor++]),z=double(vertices[cursor++]);
        double p[3]={m[0]*x+m[1]*y+m[2]*z+m[3],m[4]*x+m[5]*y+m[6]*z+m[7],m[8]*x+m[9]*y+m[10]*z+m[11]};
        for(int axis=0;axis<3;++axis){low[axis]=std::min(low[axis],p[axis]);high[axis]=std::max(high[axis],p[axis]);}
      }
    }
    if(cursor!=vertices.size())fail("Invalid bone vertex blocks");
    geometry={(low[0]+high[0])/2.0,(low[1]+high[1])/2.0,(low[2]+high[2])/2.0,
              std::max(500.0,(high[0]-low[0])/2.0),std::max(500.0,(high[1]-low[1])/2.0),
              std::max(500.0,(high[2]-low[2])/2.0)};
  }
  auto root=jobj();root->v["vertices"]=mesh_vec_i(vertices);root->v["normals"]=mesh_vec_i(normals);auto polys=jarr();auto emit_poly=[&](const Poly&p){auto o=jobj();auto ind=jarr();for(int x:p.indices)ind->v.push_back(ji(x));o->v["indices"]=ja(ind);auto at=jarr();for(int x:p.attr)at->v.push_back(ji(x));o->v["attributes"]=ja(at);o->v["texture"]=ji(p.texture);o->v["pattern"]=ji(p.pattern);o->v["blend"]=ji(p.blend);o->v["double_sided"]=p.double_sided;polys->v.push_back(jo(o));};for(auto&p:textured)emit_poly(p);for(auto&p:colored)emit_poly(p);root->v["polygons"]=ja(polys);auto barr=jarr();for(auto&b:bones){auto o=jobj();o->v["vertices"]=ji(b.vertices);o->v["parent"]=ji(b.parent);o->v["matrix"]=mesh_vec(b.matrix);barr->v.push_back(jo(o));}root->v["bones"]=ja(barr);root->v["patterns"]=ji(np);
  return {root,{{"__geometry",geometry}}};
}

static Json micro_animation(const std::vector<uint8_t>&data){
  Bits r(data);int version=r.header("MT"),actions=r.u16(),nb=r.u16();r.skip(20);if(actions>256||nb>256)fail("Animation exceeds limits");
  auto ID=[](){return std::vector<double>{1,0,0,0,0,1,0,0,0,0,1,0};};
  auto sample=[&](const std::vector<std::pair<int,std::vector<double>>>&tr,int frame){if(frame>=tr.back().first)return tr.back().second;for(int i=int(tr.size())-2;i>=0;--i)if(frame>=tr[i].first){double a=double(frame-tr[i].first)/double(tr[i+1].first-tr[i].first);std::vector<double>v;for(size_t j=0;j<tr[i].second.size();++j)v.push_back(tr[i].second[j]+(tr[i+1].second[j]-tr[i].second[j])*a);return v;}return std::vector<double>(tr[0].second.size(),0);};
  auto track=[&](int width=3,double factor=1.0){int count=r.u16();if(count<1||count>4096)fail("Invalid animation track");std::vector<std::pair<int,std::vector<double>>> v;for(int i=0;i<count;++i){int key=r.u16();std::vector<double>x;for(int j=0;j<width;++j)x.push_back(r.s16()*factor);if(i&&v.back().first>=key)fail("Unsorted animation track");v.push_back({key,x});}return v;};
  struct Bone{std::vector<double> matrix,translate,rotate,roll,scale;std::vector<std::pair<int,std::vector<double>>> tt,rr,rl,ss;};
  auto out=jarr();int total=0;
  for(int act=0;act<actions;++act){int last=r.u16();total+=(last+1)*nb*12;if(total>4000000)fail("Animation exceeds limits");std::vector<Bone>b(nb);
    for(auto &x:b){int kind=r.u8();if(kind==0)x.matrix=r.matrix();else if(kind==1)x.matrix=ID();else if(kind>=2&&kind<=6){if(kind==2||kind==6)x.tt=track();if(kind==3)x.tt={{0,{double(r.s16()),double(r.s16()),double(r.s16())}}};if(kind==2)x.ss=track(3,1.0/4096.0);x.rr=track();if(kind==3)x.rl={{0,{double(r.s16())*6.283185307179586/4096.0}}};else if(kind!=5)x.rl=track(1,6.283185307179586/4096.0);}else fail("Unsupported animation bone");}
    auto mats=jarr();for(int frame=0;frame<=last;++frame){auto all=jarr();for(auto &x:b){std::vector<double>m=x.matrix.empty()?ID():x.matrix;if(!x.tt.empty()){auto v=sample(x.tt,frame);m[3]=v[0];m[7]=v[1];m[11]=v[2];}if(!x.rr.empty()){auto v=sample(x.rr,frame);double xx=v[0],yy=v[1],zz=v[2];if(xx==0&&yy==0){if(zz<0)m[5]=m[10]=-1;}else{double len=std::sqrt(xx*xx+yy*yy+zz*zz);xx/=len;yy/=len;zz/=len;len=std::hypot(xx,yy);double rx=-yy/len,ry=xx/len;double s=std::sqrt(std::max(0.0,1-zz*zz)),nc=1-zz;m[0]=rx*rx*nc+zz;m[1]=rx*ry*nc;m[2]=ry*s;m[4]=rx*ry*nc;m[5]=ry*ry*nc+zz;m[6]=-rx*s;m[8]=-ry*s;m[9]=rx*s;m[10]=zz;}}
      if(!x.rl.empty()){double angle=sample(x.rl,frame)[0],c=std::cos(angle),s=std::sin(angle);for(int row:{0,4,8}){double a=m[row],bb=m[row+1];m[row]=a*c+bb*s;m[row+1]=bb*c-a*s;}}
      if(!x.ss.empty()){auto v=sample(x.ss,frame);for(int row:{0,4,8})for(int col=0;col<3;++col)m[row+col]*=v[col];}
      for(double v:m)all->v.push_back(jd(v));
    }mats->v.push_back(ja(all));}auto ao=jobj();ao->v["last_frame"]=ji(last);ao->v["matrices"]=ja(mats);auto patterns=jobj();if(version==5){int n=r.u16();for(int i=0;i<n;++i){std::string k=std::to_string(r.u16());patterns->v[k]=ji(r.i32());}}ao->v["patterns"]=jo(patterns);out->v.push_back(jo(ao));
  }
  return ja(out);
}

static std::vector<uint8_t> bmp_png(const std::vector<uint8_t>&d,bool alpha){
  if(d.size()<54||d[0]!='B'||d[1]!='M')fail("Invalid BMP");
  uint32_t offset=rd32(d.data()+10),header=rd32(d.data()+14);
  int32_t w=int32_t(rd32(d.data()+18)),h=int32_t(rd32(d.data()+22));
  uint16_t planes=rd16(d.data()+26),bits=rd16(d.data()+28);uint32_t comp=rd32(d.data()+30);
  if(header!=40||planes!=1||bits!=8||comp||w<=0||std::abs(h)>4096||w>4096)fail("Unsupported BMP encoding");
  uint32_t colors=rd32(d.data()+46);if(!colors)colors=256;
  if(colors<1||colors>256||offset<54+colors*4)fail("Invalid BMP palette");
  size_t stride=(size_t(w)+3)/4*4;
  if(uint64_t(offset)+uint64_t(std::abs(h))*stride>d.size())fail("Truncated BMP pixels");
  std::vector<uint8_t> rows;rows.reserve(size_t(w)*size_t(std::abs(h))*4+size_t(std::abs(h)));
  for(int y=0;y<std::abs(h);++y){rows.push_back(0);int sy=h>0?(std::abs(h)-1-y):y;const uint8_t*p=d.data()+offset+size_t(sy)*stride;for(int x=0;x<w;++x){uint8_t idx=p[x];if(idx>=colors)fail("BMP palette index outside table");const uint8_t*q=d.data()+54+size_t(idx)*4;rows.push_back(q[2]);rows.push_back(q[1]);rows.push_back(q[0]);rows.push_back(alpha&&idx==0?0:255);}}
  uLongf clen=compressBound(rows.size());std::vector<uint8_t> z(clen);if(compress2(z.data(),&clen,rows.data(),rows.size(),Z_BEST_SPEED)!=Z_OK)fail("PNG compression failed");z.resize(clen);
  std::vector<uint8_t> out;auto chunk=[&](const char*kind,const std::vector<uint8_t>&p){std::string tmp;wrbe32(tmp,uint32_t(p.size()));tmp.append(kind,4);tmp.append(reinterpret_cast<const char*>(p.data()),p.size());uint32_t c=crc32(0,reinterpret_cast<const Bytef *>(tmp.data()+4),tmp.size()-4);wrbe32(tmp,c);out.insert(out.end(),tmp.begin(),tmp.end());};
  static const uint8_t sig[]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};out.insert(out.end(),sig,sig+8);std::vector<uint8_t>ihdr={uint8_t(w>>24),uint8_t(w>>16),uint8_t(w>>8),uint8_t(w),uint8_t(std::abs(h)>>24),uint8_t(std::abs(h)>>16),uint8_t(std::abs(h)>>8),uint8_t(std::abs(h)),8,6,0,0,0};chunk("IHDR",ihdr);chunk("IDAT",z);chunk("IEND",{});return out;
}

static bool safe_name(const std::string &n){
  if(n.empty()||n[0]=='/'||n.find('\\')!=std::string::npos||n.find(':')!=std::string::npos)return false;
  static const std::string allowed="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-";
  for(char c:n)if(allowed.find(c)==std::string::npos)return false;
  size_t p=0;while(true){size_t q=n.find('/',p);std::string part=n.substr(p,q==std::string::npos?n.size()-p:q-p);if(part.empty()||part=="."||part=="..")return false;if(q==std::string::npos)break;p=q+1;}return true;
}
static void write_bin(const std::string &path,const std::vector<uint8_t>&d){mkdir_recursive(dirname_of(path));FILE*f=fopen(path.c_str(),"wb");if(!f)fail("Cannot create "+path);wrbytes(f,d.data(),d.size());fclose(f);}
static std::string read_text(const std::string &path){auto d=read_all(path);return std::string(reinterpret_cast<const char*>(d.data()),d.size());}

struct ModelReg {int id;std::string model,texture;int texture_id=-1;};

static std::vector<std::string> split(const std::string&s,char c){std::vector<std::string>o;size_t p=0;while(true){size_t q=s.find(c,p);o.push_back(s.substr(p,q==std::string::npos?s.size()-p:q-p));if(q==std::string::npos)break;p=q+1;}return o;}

static std::vector<std::vector<int64_t>> read_tables(const std::string&root,const std::string&name,bool station){
  std::string raw=read_text(root+"/data/txt/"+name+".txt");for(char &c:raw)if(c=='\r'||c=='\n'||c=='\t')c=0;std::string clean;for(char c:raw)if(c)clean.push_back(c);
  auto rows=split(clean,';');std::vector<std::vector<int64_t>>out;
  for(auto &row:rows){if(row.empty())continue;auto cells=split(row,',');std::vector<int64_t>v;size_t start=station?1:0;for(size_t i=start;i<cells.size();++i)for(auto &s:split(cells[i],' ')){if(s.empty())continue;v.push_back(std::stoll(s));}out.push_back(std::move(v));}
  return out;
}

static std::vector<std::string> station_names(const std::string&root){
  std::string raw=read_text(root+"/data/txt/stations.txt");for(char &c:raw)if(c=='\r'||c=='\n'||c=='\t')c=0;
  std::string clean;for(char c:raw)if(c)clean.push_back(c);
  std::vector<std::string>out;for(auto &row:split(clean,';')){if(row.empty())continue;auto cells=split(row,',');if(!cells.empty())out.push_back(cells[0]);}
  return out;
}

static Json table_json(const std::string&root){
  auto all=jobj();for(const auto&name:{"ships","equipment","goods","stations","creatures"}){
    std::string raw=read_text(root+"/data/txt/"+name+".txt");for(char &c:raw)if(c=='\r'||c=='\n'||c=='\t')c=0;std::string clean;for(char c:raw)if(c)clean.push_back(c);
    auto rows=split(clean,';');auto arr=jarr();bool station=!strcmp(name,"stations"),goods=!strcmp(name,"goods");
    for(auto &row:rows){if(row.empty())continue;auto cells=split(row,',');auto r=jarr();if(station){r->v.push_back(js(cells[0]));for(size_t i=1;i<cells.size();++i)r->v.push_back(ji(std::stoll(cells[i])));}
      else if(goods){for(size_t i=0;i<std::min<size_t>(7,cells.size());++i)r->v.push_back(ji(std::stoll(cells[i])));for(size_t i=7;i<cells.size();++i){auto q=jarr();for(auto&s:split(cells[i],' '))if(!s.empty())q->v.push_back(ji(std::stoll(s)));r->v.push_back(ja(q));}}
      else {for(auto&s:cells)r->v.push_back(ji(std::stoll(s)));}arr->v.push_back(ja(r));}
    all->v[name]=ja(arr);
  }return jo(all);
}

static std::map<std::string,std::vector<double>> g_model_geometry;

static Json read_lang_file(const std::string&path){
  auto d=read_all(path);Rdr r(d);auto arr=jarr();while(r.p<d.size()){uint16_t n=r.u2();auto x=r.take(n);arr->v.push_back(js(mutf8(x)));}return ja(arr);
}

static void build_profile(const std::string&jar,const std::string&root,const ZipReader&zip){
  static const char* classes_req[]={"ah","e","bo","ab","f","dj","cy"};
  std::map<std::string,ClassData> classes;
  for(const char* n:classes_req){
    if(!zip.has(std::string(n)+".class"))
      fail("This DEEP build does not match the declarative content profile: missing "+std::string(n)+".class");
    classes.emplace(n,ClassData(zip.read(std::string(n)+".class")));
  }

  StaticMap st;
  for(auto &cp:classes){
    for(auto &f:cp.second.fields){
      if(!(f.second.flags&8))continue;
      auto key=std::make_tuple(cp.first,f.first.first,f.first.second);
      auto it=f.second.attrs.find("ConstantValue");
      if(it==f.second.attrs.end()){st[key]=defval(f.first.second);continue;}
      Rdr rr(it->second);uint16_t ix=rr.u2();CP c=cp.second.constant(ix);
      if(c.tag==3||c.tag==5)st[key]=std::get<int64_t>(c.v);
      else if(c.tag==4||c.tag==6)st[key]=std::get<double>(c.v);
      else st[key]=cp.second.constant_str(ix);
    }
  }

  auto al=std::make_shared<Obj>();al->type="al";
  al->f["a:I"]=int64_t(-1);al->f["b:Z"]=false;al->f["e:Z"]=false;
  st[{"al","a","Lal;"}]=al;st[{"ap","b","I"}]=int64_t(1);

  std::map<int,std::string> textures;
  std::vector<std::string> station_names_table=station_names(root);
  std::vector<ModelReg> models;
  std::shared_ptr<Obj> active;
  std::vector<int64_t> changes;

  Evaluator ev{classes,st,[&](const std::string&owner,const std::string&name,const std::string&desc,
                              const std::shared_ptr<Obj>&obj,const std::vector<Val>&args)->Val{
    if(owner=="java/lang/StringBuffer"){
      if(name=="<init>"){
        if(obj)obj->f["text"]=args.empty()?Val(std::string("")):
          (std::holds_alternative<std::string>(args[0])?args[0]:Val(std::string("")));
        return {};
      }
      if(name=="append"){
        std::string left;
        if(obj){
          auto it=obj->f.find("text");
          if(it!=obj->f.end()&&std::holds_alternative<std::string>(it->second))left=std::get<std::string>(it->second);
        }
        std::string right=args.empty()?"":val_string(args[0]);
        if(obj)obj->f["text"]=left+right;
        return obj;
      }
      if(name=="toString"){
        if(obj&&obj->f.count("text"))return obj->f["text"];
        return Val(std::string(""));
      }
    }

    if(owner=="cd"&&name=="a"){
      if(desc=="(ILjava/lang/String;)V"){
        textures[int(as_i(args[0]))]=std::get<std::string>(args[1]);return {};
      }
      if(desc=="(ILjava/lang/String;II)V"||desc=="(ILjava/lang/String;I)V"){
        int tid=int(as_i(args.back()));
        ModelReg m{int(as_i(args[0])),std::get<std::string>(args[1]),"",tid};
        models.push_back(std::move(m));return {};
      }
    }

    if(owner=="al"){
      if(name=="<init>"&&desc=="(III)V"){
        int kind=int(as_i(args[0])),reward=int(as_i(args[1])),dest=int(as_i(args[2]));
        if(obj){
          obj->f["a:I"]=int64_t(kind);obj->f["c:I"]=int64_t(reward);obj->f["e:I"]=int64_t(dest);
          obj->f["b:Ljava/lang/String;"]=Val(dest>=0&&dest<int(station_names_table.size())?station_names_table[size_t(dest)]:"");
          obj->f["g:I"]=int64_t(-1);obj->f["h:I"]=int64_t(-1);obj->f["b:Z"]=true;obj->f["e:Z"]=true;
        }
        return {};
      }
      if(name=="a"&&desc=="(III)V"){
        if(obj){int total=int(as_i(args[1])),minimum=int(as_i(args[2]));obj->f["n:I"]=int64_t(as_i(args[0]));obj->f["o:I"]=int64_t(total);obj->f["p:I"]=int64_t(minimum);obj->f["q:I"]=int64_t(total?minimum*100/total:0);}
        return {};
      }
      if(name=="a"&&desc=="(II)V"){if(obj){obj->f["l:I"]=int64_t(as_i(args[0]));obj->f["m:I"]=int64_t(as_i(args[1]));}return {};}
    }

    if(owner=="dj"){
      if(name=="c"&&desc=="(Lal;)V"){
        active=args.empty()?nullptr:std::get<std::shared_ptr<Obj>>(args[0]);if(active)active->f["b:Z"]=true;return {};
      }
      if(name=="c"&&desc=="()Lal;")return active?Val(active):Val(std::monostate{});
      if(name=="a"&&desc=="([I)V"){
        auto a=std::get<std::shared_ptr<Arr>>(args[0]);if(a)for(auto&v:a->v)changes.push_back(as_i(v));return {};
      }
      if((name=="j"||name=="o")&&desc=="()I")return int64_t(0);
    }

    if(owner=="dt"&&name=="c"&&desc=="(II)I")return int64_t(std::min(as_i(args[0]),as_i(args[1])));

    if(owner=="dk"&&name=="<init>"&&(desc=="(IIII)V"||desc=="(III[I)V")){
      if(obj){
        obj->f["text_id:I"]=int64_t(as_i(args[0]));
        obj->f["speaker:I"]=int64_t(as_i(args[1]));
        obj->f["kind:I"]=int64_t(as_i(args[2]));
        if(std::holds_alternative<std::shared_ptr<Arr>>(args[3])) obj->f["values"]=args[3];
        else {auto values=std::make_shared<Arr>();values->v.push_back(args[3]);obj->f["values"]=Val(values);}
      }
      return {};
    }

    fail("Call is not an approved data summary: "+owner+"."+name+desc);
    return {};
  }};

  for(const char*n:{"ah","e","bo","ab","f"})ev.run(n,"<clinit>","()V",{});
  ev.run("ah","e","()V",{});

  for(auto &m:models){
    if(!m.model.empty()&&m.model[0]=='/')m.model.erase(m.model.begin());
    auto ti=textures.find(m.texture_id);if(ti==textures.end())fail("Missing registered texture");
    m.texture=ti->second;
    if(!m.texture.empty()&&m.texture[0]=='/')m.texture.erase(m.texture.begin());
    m.model+=".mbac";m.texture+=".bmp";
    if(!file_exists(root+"/"+m.model)||!file_exists(root+"/"+m.texture))
      fail("Missing registered resource: "+m.model);
  }

  auto registry=jarr();auto binds=jobj();
  for(auto&m:models){
    auto o=jobj();o->v["id"]=ji(m.id);o->v["model"]=js(m.model);o->v["texture_id"]=ji(m.texture_id);
    auto ts=jarr();ts->v.push_back(js(m.texture));o->v["textures"]=ja(ts);
    registry->v.push_back(jo(o));binds->v[m.model]=ja(ts);
  }
  std::string reg_s=json_string(ja(registry));write_bin(root+"/resource_registry.json",std::vector<uint8_t>(reg_s.begin(),reg_s.end()));
  std::string bind_s=json_string(jo(binds));write_bin(root+"/bindings.json",std::vector<uint8_t>(bind_s.begin(),bind_s.end()));

  auto constants=jobj();
  for(const char*owner:{"ah","e","bo","ab","f"}){
    auto o=jobj();
    for(auto&kv:st){
      if(std::get<0>(kv.first)!=owner)continue;
      std::string d=std::get<2>(kv.first);
      bool allowed=(d=="I"||d=="S"||d=="B"||d=="Z"||d=="J"||d=="F"||d=="D"||d=="C"||d=="Ljava/lang/String;"||(!d.empty()&&d[0]=='['));
      if(allowed&&!is_null(kv.second))o->v[std::get<1>(kv.first)+":"+desc_type(d)]=val_json(kv.second);
    }
    constants->v[owner]=jo(o);
  }

  auto sint=jarr();
  for(int i=0;i<=1024;++i)sint->v.push_back(ji(int(std::llround(std::sin(i*6.283185307179586/4096.0)*4096.0))));
  auto dt=jobj();dt->v["a:[S"]=ja(sint);constants->v["dt"]=jo(dt);

  int chapters=0;
  auto ec_it=st.find(std::make_tuple("e","g","[S"));
  if(ec_it!=st.end()&&std::holds_alternative<std::shared_ptr<Arr>>(ec_it->second)&&std::get<std::shared_ptr<Arr>>(ec_it->second))
    chapters=int(std::get<std::shared_ptr<Arr>>(ec_it->second)->v.size());
  if(chapters<=0)fail("The declarative content profile contains no campaign chapters");

  auto campaign=jarr();auto timelines=jobj();
  for(int ch=1;ch<=chapters;++ch){
    st[{"dj","v","I"}]=int64_t(ch-1);active.reset();changes.clear();
    ev.run("dj","c","()V",{});
    auto def=jobj();def->v["chapter"]=ji(ch);def->v["mission"]=record_json(active);
    auto rebels=jarr();std::set<int64_t>unique(changes.begin(),changes.end());for(int64_t x:unique)rebels->v.push_back(ji(x));
    def->v["rebel_stations"]=ja(rebels);campaign->v.push_back(jo(def));

    auto cy=std::make_shared<Obj>();cy->type="cy";auto ca=std::make_shared<Arr>();ca->v.push_back(Val(std::monostate{}));cy->f["a:[Law;"]=ca;
    Val evv=ev.run("cy","a","(I)[Ldk;",{Val(cy),int64_t(ch)});
    auto timeline=jarr();
    if(std::holds_alternative<std::shared_ptr<Arr>>(evv)){
      auto aa=std::get<std::shared_ptr<Arr>>(evv);
      if(aa)for(const auto &v:aa->v){
        if(std::holds_alternative<std::shared_ptr<Obj>>(v)){
          auto event=std::get<std::shared_ptr<Obj>>(v);auto eo=jobj();
          if(event){
            for(const char *k:{"text_id","speaker","kind","values"}){
              auto fi=event->f.find(k);if(fi!=event->f.end())eo->v[k]=val_json(fi->second);
            }
          }
          timeline->v.push_back(jo(eo));
        }
      }
    }
    if(!timeline->v.empty())timelines->v[std::to_string(ch)]=ja(timeline);
  }

  auto strings=jarr();
  std::vector<std::string>langs;DIR*d=opendir((root+"/data/lang").c_str());
  if(d){
    struct dirent*e;while((e=readdir(d))){
      if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
      std::string p=root+"/data/lang/"+e->d_name;struct stat stbuf{};
      if(stat(p.c_str(),&stbuf)==0&&S_ISDIR(stbuf.st_mode))langs.push_back(e->d_name);
    }
    closedir(d);
  }
  if(langs.empty())fail("This DEEP build does not match the declarative content profile: no localisation");
  std::sort(langs.begin(),langs.end());
  std::string lang="en";if(std::find(langs.begin(),langs.end(),"en")==langs.end())lang=langs[0];

  std::vector<std::string>lnames={"main","ships","cargo","items","medals"};
  for(int i=0;i<chapters-1;++i)lnames.push_back(std::to_string(i));
  for(auto&n:lnames){
    Json a=read_lang_file(root+"/data/lang/"+lang+"/"+n+".lang");
    auto ar=std::get<std::shared_ptr<JsonArr>>(a);for(auto &x:ar->v)strings->v.push_back(x);
  }

  auto names=jarr();
  for(const char*g:{"f","m"}){
    auto a=jarr();std::string raw=read_text(root+"/data/txt/names_human_"+g+".txt");
    for(char &c:raw)if(c=='\r'||c=='\n'||c=='\t')c=0;
    std::string clean;for(char c:raw)if(c)clean.push_back(c);
    for(auto&s:split(clean,';'))if(!s.empty())a->v.push_back(js(s));
    names->v.push_back(ja(a));
  }

  int species_count=int(read_tables(root,"creatures",false).size());
  auto habitats=jarr();
  int station_count=int(read_tables(root,"stations",true).size());
  for(int index=0;index<station_count;++index){
    auto h=jarr();
    for(int off=0;off<std::min(6,species_count);++off){
      h->v.push_back(ji((index*7+off*5)%species_count));h->v.push_back(ji(20));
    }
    habitats->v.push_back(ja(h));
  }

  auto out=jobj();
  out->v["schema"]=ji(1);out->v["jar_sha256"]=js(sha256_file(jar));out->v["importer"]=js("native-6");
  out->v["language"]=js(lang);out->v["constants"]=jo(constants);out->v["tables"]=table_json(root);
  out->v["campaign"]=ja(campaign);out->v["timelines"]=jo(timelines);out->v["strings"]=ja(strings);
  out->v["name_pools"]=ja(names);out->v["habitats"]=ja(habitats);out->v["data_reader"]=js("restricted-class-data-1");
  auto station_geometry=jobj();
  for(const auto &m:models){
    if(m.id<3300||m.id>=3400)continue;
    auto it=g_model_geometry.find(m.model);if(it==g_model_geometry.end()||it->second.size()!=6)continue;
    auto g=jobj();auto c=jarr();auto e=jarr();
    for(int i=0;i<3;++i)c->v.push_back(jd(it->second[i]));
    for(int i=3;i<6;++i)e->v.push_back(jd(it->second[i]));
    g->v["center"]=ja(c);g->v["extent"]=ja(e);station_geometry->v[std::to_string(m.id)]=jo(g);
  }
  out->v["station_geometry"]=jo(station_geometry);
  std::string s=json_string(jo(out));write_bin(root+"/native-data.json",std::vector<uint8_t>(s.begin(),s.end()));
  debugPrintf("[jar] profile: models=%zu chapters=%d strings=%zu language=%s\\n", models.size(), chapters, strings->v.size(), lang.c_str());
}

static void extract_jar(const std::string&jar,const std::string&root){
  g_model_geometry.clear();ZipReader z(jar);std::string manifest;
  try{auto m=z.read("META-INF/MANIFEST.MF");manifest=std::string(reinterpret_cast<const char*>(m.data()),m.size());}catch(...){fail("Unsupported JAR: not a readable MIDlet archive.");}
  for(size_t p=0;(p=manifest.find("\r\n",p))!=std::string::npos;){manifest.replace(p,2,"\n");}
  for(size_t p=0;(p=manifest.find("\n ",p))!=std::string::npos;)manifest.erase(p+0,1);
  std::map<std::string,std::string>fields;for(auto&line:split(manifest,'\n')){size_t q=line.find(": ");if(q!=std::string::npos)fields[line.substr(0,q)]=line.substr(q+2);}
  auto mid=split(fields["MIDlet-1"],',');if(mid.size()!=3||mid[2]!="DeepMIDlet")fail("Unsupported JAR: this is not a DEEP MIDlet.");
  std::string icon=mid[1];if(!icon.empty()&&icon[0]=='/')icon.erase(icon.begin());
  mkdir_recursive(root);
  auto names=z.names();size_t total=0;
  for(size_t i=0;i<names.size();++i){const std::string&n=names[i];auto it=z.entries.find(n);if(it==z.entries.end()||n=="META-INF/MANIFEST.MF"||n.empty()||n.back()=='/')continue;if(!safe_name(n))fail("Unsafe JAR entry");if(n.rfind("data/",0)!=0)continue;if(uint64_t(total)+it->second.size>128u*1024u*1024u)fail("JAR exceeds import limits");auto d=z.read(n);total+=d.size();std::string out=root+"/"+n;if(n!=icon){std::string ext=n.substr(n.find_last_of('.')+1);if(ext=="mbac"||ext=="mtra"||ext=="bmp"||ext=="png"){std::vector<uint8_t>u=d;int sz=int(u.size());int count=sz<100?10+sz%10:sz<200?50+sz%20:sz<300?80+sz%20:100+sz%50;if(sz<count)fail("Resource envelope is too short");for(int k=0;k<count;++k)std::swap(u[size_t(k)],u[size_t(sz-1-k)]);d.swap(u);}}write_bin(out,d);
    if(n.size()>=5&&n.substr(n.size()-5)==".mbac"){auto m=micro_model(d);if(!m.geometry.empty())g_model_geometry[n]=m.geometry["__geometry"];std::string s=json_string(jo(m.json));write_bin(out+".json",std::vector<uint8_t>(s.begin(),s.end()));}
    else if(n.size()>=5&&n.substr(n.size()-5)==".mtra"){Json a=micro_animation(d);std::string s=json_string(a);write_bin(out+".json",std::vector<uint8_t>(s.begin(),s.end()));}
    else if(n.size()>=4&&n.substr(n.size()-4)==".bmp"){auto p=bmp_png(d,false),pa=bmp_png(d,true);write_bin(out+".png",p);write_bin(out+".alpha.png",pa);}
  }
  build_profile(jar,root,z);
}

static void collect_files(const std::string&dir,const std::string&rel,std::vector<std::string>&out){
  DIR*d=opendir(dir.c_str());if(!d)return;struct dirent*e;while((e=readdir(d))){if(e->d_name[0]=='.')continue;std::string full=dir+"/"+e->d_name,r=rel.empty()?e->d_name:rel+"/"+e->d_name;struct stat st{};if(stat(full.c_str(),&st))continue;if(S_ISDIR(st.st_mode))collect_files(full,r,out);else out.push_back(r);}closedir(d);
}

static void validate_generated_pack(const std::string &pack,const std::string &root){
  auto raw=read_all(pack,128u*1024u*1024u);
  if(raw.size()<22)fail("Generated content pack is too small");
  size_t end=raw.size()-22;
  if(rd32(raw.data()+end)!=0x06054b50u)fail("Generated content pack has no ZIP end record");
  if(rd16(raw.data()+end+4)!=0||rd16(raw.data()+end+6)!=0||rd16(raw.data()+end+20)!=0)fail("Generated content pack uses unsupported ZIP features");
  uint16_t count=rd16(raw.data()+end+10);
  uint32_t cdsize=rd32(raw.data()+end+12),cd=rd32(raw.data()+end+16);
  if(count<4||count>4096||rd16(raw.data()+end+8)!=count)fail("Generated content pack has an invalid entry count");
  if(uint64_t(cd)+cdsize!=end||uint64_t(cd)+cdsize>raw.size())fail("Generated content pack central directory is invalid");
  size_t p=cd;bool have_pack=false,have_native=false,have_registry=false,have_bindings=false;
  for(uint16_t i=0;i<count;++i){
    if(p+46>end||rd32(raw.data()+p)!=0x02014b50u)fail("Generated content pack central directory entry is invalid");
    uint16_t flags=rd16(raw.data()+p+8),method=rd16(raw.data()+p+10);
    uint32_t expanded=rd32(raw.data()+p+24);
    uint16_t nl=rd16(raw.data()+p+28),el=rd16(raw.data()+p+30),cl=rd16(raw.data()+p+32);
    if(flags&1||method!=0||expanded>32u*1024u*1024u||p+46+nl+el+cl>end)fail("Generated content pack has an unsupported entry");
    std::string name(reinterpret_cast<const char*>(raw.data()+p+46),nl);
    if(!safe_name(name))fail("Generated content pack contains an unsafe entry");
    if(name=="pack.json")have_pack=true;else if(name=="native-data.json")have_native=true;else if(name=="resource_registry.json")have_registry=true;else if(name=="bindings.json")have_bindings=true;
    p+=46+nl+el+cl;
  }
  if(p!=end||!have_pack||!have_native||!have_registry||!have_bindings)fail("Generated content pack is missing required files");
  auto native=read_text(root+"/native-data.json");
  auto registry=read_text(root+"/resource_registry.json");
  auto bindings=read_text(root+"/bindings.json");
  if(native.find("\\"schema\\":1")==std::string::npos||native.find("\\"jar_sha256\\":\\""+jar_sha+"\\"")==std::string::npos||native.find("\\"importer\\":\\"native-6\\"")==std::string::npos)fail("Generated native-data.json is incomplete");
  if(native.find("\\"campaign\\":[")==std::string::npos||native.find("\\"strings\\":[")==std::string::npos)fail("Generated native-data.json has no campaign/string data");
  if(registry.size()<3||registry.find_first_not_of(" \t\r\n")!=0||registry.find("[ ]")!=std::string::npos||registry=="[]")fail("Generated resource registry is empty");
  if(bindings.size()<3||bindings=="{}")fail("Generated resource bindings are empty");
  debugPrintf("[jar] generated pack validated: entries=%u size=%zu registry=%zu\n",(unsigned)count,raw.size(),registry.size());
}

static void build_pack(const std::string&root,const std::string&jar_sha,const std::string&pack){
  std::vector<std::string> names={"native-data.json","resource_registry.json","bindings.json"};std::vector<std::string>all;collect_files(root+"/data","data",all);names.insert(names.end(),all.begin(),all.end());
  std::vector<PackItem> items;auto man=jobj();man->v["format"]=js("abyssal-content-1");man->v["profile"]=js(jar_sha);auto files=jobj();
  for(auto&name:names){if(name.size()>=4){std::string e=name.substr(name.find_last_of('.')+1);if(e=="amr"||e=="mid")continue;}std::vector<uint8_t>d=read_all(root+"/"+name,32u*1024u*1024u);if(d.size()>32u*1024u*1024u)fail("Decoded content exceeds limits");auto rec=jobj();rec->v["size"]=ji(int64_t(d.size()));rec->v["sha256"]=js(sha256(d));files->v[name]=jo(rec);items.push_back({name,std::move(d),0,0});}
  man->v["files"]=jo(files);std::string ms=json_string(jo(man));items.insert(items.begin(),PackItem{"pack.json",std::vector<uint8_t>(ms.begin(),ms.end()),0,0});if(items.size()>4096)fail("Too many decoded resources");write_zip_stored(pack,std::move(items));
}

static int prepare(const char*jar_path,const char*cache_root,char*out,unsigned out_size){
  if(!jar_path||!cache_root||!out||!out_size){g_error="Invalid importer arguments";return 0;}
  try{
    long size=0;FILE*f=fopen(jar_path,"rb");if(!f)fail("Cannot open JAR");fseek(f,0,SEEK_END);size=ftell(f);fclose(f);if(size<0||size>16*1024*1024)fail("JAR exceeds 16 MiB.");
    std::string digest=sha256_file(jar_path);std::string base=std::string(cache_root)+"/_jar_import_v3";std::string pack=base+"/"+digest+".abyss";
    if(!file_exists(pack)){std::string work=base+"/"+digest+".work";remove_tree(work);mkdir_recursive(work);extract_jar(jar_path,work);build_pack(work,digest,pack);validate_generated_pack(pack,work);remove_tree(work);}
    if(!file_exists(pack))fail("Native JAR converter did not create a content pack");
    if(pack.size()+1>out_size)fail("Converted pack path is too long");
    snprintf(out,out_size,"%s",pack.c_str());
    g_error.clear();return 1;
  }catch(const std::exception&e){g_error=e.what();debugPrintf("[jar] %s\n",g_error.c_str());return 0;}
}

} 

extern "C" int jar_import_prepare(const char*jar_path,const char*cache_root,char*out_path,unsigned out_size){
  return prepare(jar_path,cache_root,out_path,out_size);
}
extern "C" const char *jar_import_error(void){return g_error.c_str();}
