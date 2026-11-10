// Exact integer benchmarks. Explicit backends never call the paired oracle.
#include "pfl/pfl.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <sys/resource.h>
#endif

using U64 = std::uint64_t;
using Score = pfl::Score;
using State = std::uint32_t;
using Bits = std::vector<U64>;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
U64 peak_rss() {
#ifndef _WIN32
    struct rusage usage{};getrusage(RUSAGE_SELF,&usage);return U64(usage.ru_maxrss)*1024;
#else
    return 0;
#endif
}
struct BitsHash { std::size_t operator()(const Bits& x) const noexcept {
    U64 h=1469598103934665603ULL; for(U64 a:x) { h^=a; h*=1099511628211ULL; } return h;
}};
bool subset(const Bits& a,const Bits& b) { for(std::size_t i=0;i<a.size();++i) if(a[i]&~b[i]) return false; return true; }
std::size_t cardinality(const Bits& a) {std::size_t n=0;for(U64 x:a)n+=__builtin_popcountll(x);return n;}
void bit_or(Bits& a,const Bits& b) {for(std::size_t i=0;i<a.size();++i)a[i]|=b[i];}
struct Event { U64 mass; std::vector<int> domains; };
struct Option { unsigned cost; std::vector<int> replicas; };
struct Input {
    std::vector<int> domain_parent,parent,order;
    std::vector<std::vector<int>> children;
    std::vector<U64> benefit;
    std::vector<std::vector<Option>> menus;
    std::vector<Event> events;
    unsigned budget=0; int root=-1; U64 mass=0;
};
std::vector<int> tree_order(const std::vector<int>& p,std::vector<std::vector<int>>& c,int& root) {
    c.assign(p.size(),{});root=-1;
    for(std::size_t i=0;i<p.size();++i) {if(p[i]<0){if(root>=0)throw std::runtime_error("multiple roots");root=int(i);}else{if(std::size_t(p[i])>=p.size())throw std::runtime_error("parent out of range");c[p[i]].push_back(int(i));}}
    if(root<0)throw std::runtime_error("missing root");
    std::vector<int> order{root};for(std::size_t i=0;i<order.size();++i)for(int child:c[order[i]])order.push_back(child);
    if(order.size()!=p.size())throw std::runtime_error("not a rooted tree");return order;
}
Input load_input(const std::string& path) {
    std::ifstream f(path);std::string magic;int version;std::size_t m,n,q;Input a;
    if(!(f>>magic>>version>>m>>n>>a.budget>>q)||magic!="PFL_R1"||version!=1||!m||!n||!q)throw std::runtime_error("bad input header");
    a.domain_parent.resize(m);a.parent.resize(n);a.benefit.resize(n);a.menus.resize(n);
    for(auto& x:a.domain_parent)f>>x;for(auto& x:a.parent)f>>x;for(auto& x:a.benefit)f>>x;
    for(auto& row:a.menus){std::size_t k;f>>k;row.resize(k);for(auto& o:row){std::size_t r;f>>o.cost>>r;o.replicas.resize(r);for(int& x:o.replicas){f>>x;if(x<0||std::size_t(x)>=m)throw std::runtime_error("replica out of range");}}if(row.empty()||row[0].cost||!row[0].replicas.empty())throw std::runtime_error("first menu option must be null");}
    a.events.resize(q);for(auto& e:a.events){std::size_t d;f>>e.mass>>d;e.domains.resize(d);for(int& x:e.domains){f>>x;if(x<0||std::size_t(x)>=m)throw std::runtime_error("event domain out of range");}if(e.mass>std::numeric_limits<U64>::max()-a.mass)throw std::runtime_error("event mass overflow");a.mass+=e.mass;}
    if(!f||!a.mass)throw std::runtime_error("incomplete input or zero mass");
    a.order=tree_order(a.parent,a.children,a.root);std::vector<std::vector<int>> dc;int dr;tree_order(a.domain_parent,dc,dr);
    for(auto& row:a.menus)for(auto& o:row)for(int r:o.replicas)if(!dc[r].empty())throw std::runtime_error("replicas must be leaves");
    return a;
}
struct LeafData {
    std::vector<Bits> clusters;std::vector<int> leaves,leaf_index;std::size_t words;
    explicit LeafData(const Input& a) {
        std::vector<std::vector<int>> children;int root;auto order=tree_order(a.domain_parent,children,root);
        leaf_index.assign(order.size(),-1);for(std::size_t i=0;i<children.size();++i)if(children[i].empty()){leaf_index[i]=int(leaves.size());leaves.push_back(int(i));}
        words=(leaves.size()+63)/64;clusters.assign(order.size(),Bits(words,0));
        for(std::size_t i=0;i<leaves.size();++i)clusters[leaves[i]][i/64]|=U64(1)<<(i%64);
        for(auto it=order.rbegin();it!=order.rend();++it)for(int child:children[*it])bit_or(clusters[*it],clusters[child]);
    }
    Bits mask(const std::vector<int>& nodes) const {Bits b(words,0);for(int x:nodes)bit_or(b,clusters[x]);return b;}
};
struct Backend {
    const Input& input;U64 merges=0;explicit Backend(const Input& x):input(x){}virtual ~Backend()=default;
    virtual State encode(const std::vector<int>&)=0;
    virtual State merge(State,State)=0;
    virtual U64 loss(State)=0;
    virtual U64 bytes()const=0;
    virtual std::size_t states()const=0;
    virtual std::vector<State> universe(){throw std::runtime_error("universal states unavailable for this backend");}
};
struct PairedBackend:Backend {
    pfl::ReducedTree tree;pfl::LatticeOracle oracle;pfl::AggregatedMasses masses;
    std::unique_ptr<pfl::DenseLossMassCompiler> dense;
    std::unordered_map<pfl::Label,State,pfl::LabelHash> ids;
    std::vector<pfl::Label> labels;std::vector<U64> losses;
    PairedBackend(const Input& a,bool dense_mode,bool envelope):Backend(a),tree(a.domain_parent),oracle(tree){
        std::vector<pfl::RawEvent> raw;std::unique_ptr<LeafData> leaf_data;
        for(auto& e:a.events){auto label=pfl::Label::bottom();for(int d:e.domains)label=oracle.join(label,pfl::Label::single(d));
            if(e.domains.size()>2&&!envelope){if(!leaf_data)leaf_data=std::make_unique<LeafData>(a);if(leaf_data->mask(e.domains)!=leaf_data->mask(oracle.components(label)))throw std::runtime_error("UNSUPPORTED_HIGHER_ORDER: use scenario backend for the complete support");}
            raw.push_back({oracle.components(label),e.mass});}
        masses=pfl::aggregate_raw_events(oracle,raw);if(dense_mode)dense=std::make_unique<pfl::DenseLossMassCompiler>(oracle,masses);intern(pfl::Label::bottom());
    }
    State intern(pfl::Label x){auto it=ids.find(x);if(it!=ids.end())return it->second;State id=State(labels.size());labels.push_back(x);ids.emplace(x,id);U64 l=0;if(dense)l=dense->loss_mass(x);else for(auto& p:masses.by_region)if(oracle.order(x,p.first).value())l+=p.second;losses.push_back(l);return id;}
    State encode(const std::vector<int>& x)override{return intern(oracle.hull_of_leaves(x));}
    State merge(State a,State b)override{++merges;return intern(oracle.join(labels[a],labels[b]));}
    U64 loss(State a)override{return losses[a];}
    U64 bytes()const override{return tree.structural_bytes()+(dense?dense->storage_bytes():0)+labels.capacity()*(sizeof(pfl::Label)+sizeof(U64));}
    std::size_t states()const override{return labels.size();}
    std::vector<State> universe()override{for(std::size_t i=0;i<tree.size();++i){intern(pfl::Label::single(int(i)));for(std::size_t j=i+1;j<tree.size();++j)intern(oracle.normalize({int(i),int(j)}));}std::vector<State> u(labels.size());std::iota(u.begin(),u.end(),0);return u;}
};
struct SetBackend:Backend {
    enum Kind{Scenario,Replica,Explicit};Kind kind;LeafData leaf;
    std::vector<Bits> event_masks,leaf_events,values;std::vector<U64> event_mass,losses;
    std::unordered_map<Bits,State,BitsHash> ids;std::vector<State> catalog_order;std::size_t event_words=0;
    explicit SetBackend(const Input& a,Kind k):Backend(a),kind(k),leaf(a){
        std::map<Bits,U64> aggregated;for(auto& e:a.events)aggregated[leaf.mask(e.domains)]+=e.mass;
        for(auto& e:aggregated){event_masks.push_back(e.first);event_mass.push_back(e.second);}event_words=(event_masks.size()+63)/64;
        if(kind==Scenario){leaf_events.assign(leaf.leaves.size(),Bits(event_words,0));for(std::size_t j=0;j<event_masks.size();++j)for(std::size_t i=0;i<leaf.leaves.size();++i)if(event_masks[j][i/64]&(U64(1)<<(i%64)))leaf_events[i][j/64]|=U64(1)<<(j%64);Bits all(event_words,~U64(0));if(event_masks.size()%64)all.back()=(U64(1)<<(event_masks.size()%64))-1;intern(all);}
        else{intern(Bits(leaf.words,0));if(kind==Explicit){for(auto& x:leaf.clusters)intern(x);for(std::size_t i=0;i<leaf.clusters.size();++i)for(std::size_t j=i+1;j<leaf.clusters.size();++j){Bits x=leaf.clusters[i];bit_or(x,leaf.clusters[j]);intern(x);}catalog_order.resize(values.size());std::iota(catalog_order.begin(),catalog_order.end(),0);std::stable_sort(catalog_order.begin(),catalog_order.end(),[&](State x,State y){return cardinality(values[x])<cardinality(values[y]);});}}
    }
    State intern(const Bits& x){auto it=ids.find(x);if(it!=ids.end())return it->second;if(values.size()>=std::numeric_limits<State>::max())throw std::runtime_error("state identifier exhausted");State id=State(values.size());values.push_back(x);ids.emplace(x,id);U64 l=0;if(kind==Scenario){for(std::size_t i=0;i<event_masks.size();++i)if(x[i/64]&(U64(1)<<(i%64)))l+=event_mass[i];}else for(std::size_t i=0;i<event_masks.size();++i)if(subset(x,event_masks[i]))l+=event_mass[i];losses.push_back(l);return id;}
    State closed_cover(const Bits& x){for(State id:catalog_order)if(subset(x,values[id]))return id;throw std::runtime_error("no closed cover");}
    State encode(const std::vector<int>& x)override{if(kind==Scenario){Bits b=values[0];for(int v:x){const auto& r=leaf_events[leaf.leaf_index[v]];for(std::size_t i=0;i<b.size();++i)b[i]&=r[i];}return intern(b);}Bits b=leaf.mask(x);return kind==Explicit?closed_cover(b):intern(b);}
    State merge(State a,State b)override{++merges;Bits x=values[a];if(kind==Scenario){for(std::size_t i=0;i<x.size();++i)x[i]&=values[b][i];}else bit_or(x,values[b]);return kind==Explicit?closed_cover(x):intern(x);}
    U64 loss(State a)override{return losses[a];}
    U64 bytes()const override{U64 n=0;for(auto* v:{&leaf.clusters,&event_masks,&leaf_events,&values})for(auto& x:*v)n+=x.capacity()*sizeof(U64);for(auto& x:values)n+=x.capacity()*sizeof(U64);return n+losses.capacity()*sizeof(U64);}
    std::size_t states()const override{return values.size();}
    std::vector<State> universe()override{if(kind!=Explicit)return Backend::universe();return catalog_order;}
};

struct MenuEntry{unsigned cost;State state;int original;};
struct NodeDP{std::vector<State> inherited,produced;std::vector<MenuEntry> menu;std::vector<std::size_t> transitions;std::vector<Score> value;};
struct Result{Score objective=0;std::vector<int> selection;U64 cost=0,cells=0,convolutions=0,transitions=0,compressed_options=0;};
Score evaluate(const Input& a,Backend& backend,const std::vector<int>& selected){std::vector<State> path(a.parent.size());Score objective=0;for(int v:a.order){State p=a.parent[v]<0?0:path[a.parent[v]];path[v]=backend.merge(p,backend.encode(a.menus[v][selected[v]].replicas));objective+=Score(a.benefit[v])*(a.mass-backend.loss(path[v]));}return objective;}
Score direct_evaluate(const Input& a,const std::vector<int>& selected){LeafData leaf(a);std::vector<Bits> paths(a.parent.size(),Bits(leaf.words));std::vector<Bits> events;for(auto& e:a.events)events.push_back(leaf.mask(e.domains));Score score=0;for(int v:a.order){if(a.parent[v]>=0)paths[v]=paths[a.parent[v]];bit_or(paths[v],leaf.mask(a.menus[v][selected[v]].replicas));if(!cardinality(paths[v]))continue;U64 saved=0;for(std::size_t j=0;j<a.events.size();++j)if(!subset(paths[v],events[j]))saved+=a.events[j].mass;score+=Score(a.benefit[v])*saved;}return score;}

Result solve_dp(const Input& a,Backend& backend,bool compress,bool reachable){
    const std::size_t stride=a.budget+1;std::vector<NodeDP> nodes(a.parent.size());Result result;result.selection.assign(a.parent.size(),0);
    std::vector<State> all;if(!reachable)all=backend.universe();nodes[a.root].inherited=reachable?std::vector<State>{0}:all;
    for(int v:a.order){auto& d=nodes[v];std::map<State,MenuEntry> minimum;
        for(std::size_t i=0;i<a.menus[v].size();++i){auto& o=a.menus[v][i];if(o.cost>a.budget)continue;State s=backend.encode(o.replicas);MenuEntry e{o.cost,s,int(i)};if(compress){auto it=minimum.find(s);if(it==minimum.end()||o.cost<it->second.cost)minimum[s]=e;}else d.menu.push_back(e);}
        if(compress)for(auto& p:minimum)d.menu.push_back(p.second);
        std::stable_sort(d.menu.begin(),d.menu.end(),[](auto& x,auto& y){return std::tie(x.cost,x.original)<std::tie(y.cost,y.original);});result.compressed_options+=d.menu.size();
        std::unordered_map<State,std::size_t> idx;if(!reachable){d.produced=all;for(std::size_t i=0;i<all.size();++i)idx[all[i]]=i;}
        for(State old:d.inherited)for(auto& o:d.menu){State s=backend.merge(old,o.state);auto it=idx.find(s);std::size_t j;if(it==idx.end()){j=d.produced.size();d.produced.push_back(s);idx.emplace(s,j);}else j=it->second;d.transitions.push_back(j);}
        result.transitions+=d.transitions.size();for(int c:a.children[v])nodes[c].inherited=reachable?d.produced:all;
        if(d.inherited.size()>std::numeric_limits<std::size_t>::max()/stride)throw std::bad_alloc();result.cells+=d.inherited.size()*stride;
    }
    for(auto vit=a.order.rbegin();vit!=a.order.rend();++vit){int v=*vit;auto& d=nodes[v];std::vector<Score> h(d.produced.size()*stride,0),next(h.size());
        for(int c:a.children[v]){auto& child=nodes[c];std::unordered_map<State,std::size_t> child_index;if(!reachable)for(std::size_t i=0;i<child.inherited.size();++i)child_index[child.inherited[i]]=i;
            for(std::size_t z=0;z<d.produced.size();++z){std::size_t ci=reachable?z:child_index.at(d.produced[z]);for(unsigned b=0;b<=a.budget;++b){Score best=0;for(unsigned x=0;x<=b;++x){Score t=h[z*stride+b-x]+child.value[ci*stride+x];if(t>best)best=t;}next[z*stride+b]=best;result.convolutions+=b+1;}}h.swap(next);
        }
        d.value.assign(d.inherited.size()*stride,0);
        for(std::size_t i=0;i<d.inherited.size();++i)for(std::size_t j=0;j<d.menu.size();++j){const auto& o=d.menu[j];std::size_t z=d.transitions[i*d.menu.size()+j];Score own=Score(a.benefit[v])*(a.mass-backend.loss(d.produced[z]));for(unsigned b=o.cost;b<=a.budget;++b){Score t=own+h[z*stride+b-o.cost];auto& best=d.value[i*stride+b];if(t>best)best=t;}}
    }
    auto root_it=std::find(nodes[a.root].inherited.begin(),nodes[a.root].inherited.end(),0);result.objective=nodes[a.root].value[std::size_t(root_it-nodes[a.root].inherited.begin())*stride+a.budget];
    struct Pending{int v;State inherited;unsigned budget;};std::vector<Pending> work{{a.root,0,a.budget}};
    while(!work.empty()){auto item=work.back();work.pop_back();auto& d=nodes[item.v];std::size_t ai=std::find(d.inherited.begin(),d.inherited.end(),item.inherited)-d.inherited.begin();Score target=d.value[ai*stride+item.budget];bool found=false;
        for(std::size_t oi=0;oi<d.menu.size()&&!found;++oi){auto& o=d.menu[oi];if(o.cost>item.budget)continue;State state=d.produced[d.transitions[ai*d.menu.size()+oi]];unsigned remaining=item.budget-o.cost;std::vector<std::vector<Score>> conv(a.children[item.v].size()+1,std::vector<Score>(remaining+1,0));std::vector<std::size_t> indices;
            for(std::size_t j=0;j<a.children[item.v].size();++j){auto& c=nodes[a.children[item.v][j]];std::size_t ci=std::find(c.inherited.begin(),c.inherited.end(),state)-c.inherited.begin();if(ci==c.inherited.size())throw std::runtime_error("reconstruction state missing");indices.push_back(ci);for(unsigned b=0;b<=remaining;++b)for(unsigned x=0;x<=b;++x)conv[j+1][b]=std::max(conv[j+1][b],conv[j][b-x]+c.value[ci*stride+x]);}
            Score own=Score(a.benefit[item.v])*(a.mass-backend.loss(state));if(own+conv.back()[remaining]!=target)continue;found=true;result.selection[item.v]=o.original;result.cost+=o.cost;
            for(std::size_t j=a.children[item.v].size();j>0;--j){int child=a.children[item.v][j-1];unsigned selected=0;while(selected<=remaining&&conv[j-1][remaining-selected]+nodes[child].value[indices[j-1]*stride+selected]!=conv[j][remaining])++selected;if(selected>remaining)throw std::runtime_error("budget reconstruction failed");work.push_back({child,state,selected});remaining-=selected;}
        }if(!found)throw std::runtime_error("option reconstruction failed");
    }if(result.cost>a.budget)throw std::runtime_error("reconstructed deployment exceeds budget");return result;
}

Result solve_greedy(const Input& a,Backend& backend){Result r;r.selection.assign(a.parent.size(),0);std::vector<int> best_single=r.selection;Score best_single_value=0;
    for(std::size_t v=0;v<a.parent.size();++v)for(std::size_t o=1;o<a.menus[v].size();++o)if(a.menus[v][o].cost<=a.budget){r.selection[v]=int(o);Score val=evaluate(a,backend,r.selection);if(val>best_single_value){best_single_value=val;best_single=r.selection;}r.selection[v]=0;}
    while(true){int bv=-1,bo=-1;Score bgain=0;std::int64_t bcost=0;
        for(std::size_t v=0;v<a.parent.size();++v)for(std::size_t o=0;o<a.menus[v].size();++o){int old=r.selection[v];std::int64_t cost=std::int64_t(a.menus[v][o].cost)-a.menus[v][old].cost;if(std::int64_t(r.cost)+cost>std::int64_t(a.budget))continue;r.selection[v]=int(o);Score val=evaluate(a,backend,r.selection);r.selection[v]=old;if(val<=r.objective)continue;Score gain=val-r.objective;bool better=bv<0||(cost<=0&&bcost>0)||(cost<=0&&bcost<=0&&gain>bgain)||(cost>0&&bcost>0&&gain*U64(bcost)>bgain*U64(cost));if(better){bv=int(v);bo=int(o);bgain=gain;bcost=cost;}}
        if(bv<0)break;r.selection[bv]=bo;r.cost=U64(std::int64_t(r.cost)+bcost);r.objective+=bgain;
    }if(best_single_value>r.objective){r.selection=best_single;r.objective=best_single_value;r.cost=0;for(std::size_t v=0;v<a.parent.size();++v)r.cost+=a.menus[v][r.selection[v]].cost;}return r;
}

std::string escape(const std::string& s){std::string out;for(char c:s){if(c=='"'||c=='\\')out+='\\';if(c=='\n')out+="\\n";else out+=c;}return out;}
int main(int argc,char** argv){
    if(argc<3){std::cerr<<"usage: revision_benchmark INPUT METHOD [no_compression|full_states]\n";return 2;}
    try{auto begin=Clock::now();Input a=load_input(argv[1]);double read_ms=ms(begin);auto total_start=Clock::now(),prep=Clock::now();std::string method=argv[2];std::unique_ptr<Backend> backend;
        if(method=="paired_dense"||method=="paired_sparse"||method=="paired_envelope")backend=std::make_unique<PairedBackend>(a,method!="paired_sparse",method=="paired_envelope");
        else if(method=="scenario"||method=="greedy")backend=std::make_unique<SetBackend>(a,SetBackend::Scenario);
        else if(method=="replica")backend=std::make_unique<SetBackend>(a,SetBackend::Replica);
        else if(method=="explicit")backend=std::make_unique<SetBackend>(a,SetBackend::Explicit);
        else throw std::runtime_error("unknown method");
        double prep_ms=ms(prep);bool compress=true,reachable=true;for(int i=3;i<argc;++i){std::string mode=argv[i];if(mode=="no_compression")compress=false;else if(mode=="full_states")reachable=false;else throw std::runtime_error("unknown ablation");}
        auto planning=Clock::now();Result result=method=="greedy"?solve_greedy(a,*backend):solve_dp(a,*backend,compress,reachable);double planning_ms=ms(planning),total_ms=ms(total_start);U64 rss=peak_rss();auto verify=Clock::now();Score direct=direct_evaluate(a,result.selection);double verify_ms=ms(verify);if(method!="paired_envelope"&&direct!=result.objective)throw std::runtime_error("independent leaf-set objective mismatch");
        std::cout<<"{\"status\":\"COMPLETE\",\"method\":\""<<method<<"\",\"objective_numerator\":\""<<pfl::score_to_string(direct)<<"\",\"model_objective_numerator\":\""<<pfl::score_to_string(result.objective)<<"\",\"probability_denominator\":"<<a.mass<<",\"cost\":"<<result.cost<<",\"read_ms\":"<<read_ms<<",\"preprocessing_ms\":"<<prep_ms<<",\"planning_reconstruction_ms\":"<<planning_ms<<",\"end_to_end_ms\":"<<total_ms<<",\"independent_verification_ms\":"<<verify_ms<<",\"peak_rss_bytes\":"<<rss<<",\"backend_core_bytes\":"<<backend->bytes()<<",\"backend_states\":"<<backend->states()<<",\"merge_calls\":"<<backend->merges<<",\"score_cells\":"<<result.cells<<",\"state_option_transitions\":"<<result.transitions<<",\"budget_convolutions\":"<<result.convolutions<<",\"compressed_options\":"<<result.compressed_options<<",\"selection\":[";
        for(std::size_t i=0;i<result.selection.size();++i){if(i)std::cout<<',';std::cout<<result.selection[i];}std::cout<<"]}\n";return 0;
    }catch(const std::bad_alloc&){std::cout<<"{\"status\":\"MEMORY_LIMIT\"}\n";return 3;}catch(const std::exception& e){std::cout<<"{\"status\":\"ERROR\",\"message\":\""<<escape(e.what())<<"\"}\n";return 1;}
}
