// Reuse the independently implemented backends, without invoking the optimizer.
#define main recovery_benchmark_main
#include "revision_benchmark.cpp"
#undef main
#include <random>

int main(int argc,char** argv) {
    if(argc<3) return 2;
    try {
        auto start=Clock::now();Input a=load_input(argv[1]);double read=ms(start);
        std::string method=argv[2];unsigned count=argc>3?unsigned(std::stoul(argv[3])):1024;
        auto total=Clock::now();std::unique_ptr<Backend> b;
        if(method=="paired_dense"||method=="paired_sparse")b=std::make_unique<PairedBackend>(a,method=="paired_dense",false);
        else if(method=="scenario")b=std::make_unique<SetBackend>(a,SetBackend::Scenario);
        else if(method=="replica")b=std::make_unique<SetBackend>(a,SetBackend::Replica);
        else if(method=="explicit")b=std::make_unique<SetBackend>(a,SetBackend::Explicit);
        else throw std::runtime_error("unknown backend");
        double preprocessing=ms(total);LeafData leaf(a);
        struct Query{std::vector<int> x,y;};std::vector<Query> queries;
        // Identical query stream for every method; generation is excluded from query time.
        std::mt19937_64 rng(102420261109ULL);
        for(unsigned i=0;i<count;++i){Query q;for(int side=0;side<2;++side){auto& v=side?q.y:q.x;auto n=rng()%5;for(unsigned j=0;j<n;++j)v.push_back(leaf.leaves[rng()%leaf.leaves.size()]);}queries.push_back(q);}
        std::vector<U64> observed;observed.reserve(count);auto query_start=Clock::now();
        Score checksum=0;
        for(auto& q:queries){State x=b->encode(q.x),y=b->encode(q.y);U64 loss=b->loss(b->merge(x,y));observed.push_back(loss);checksum+=loss;}
        double query_ms=ms(query_start);U64 rss=peak_rss();
        auto check_start=Clock::now();std::map<Bits,U64> events;for(auto& e:a.events)events[leaf.mask(e.domains)]+=e.mass;
        for(unsigned i=0;i<count;++i){Bits x=leaf.mask(queries[i].x);bit_or(x,leaf.mask(queries[i].y));U64 expected=0;for(auto& e:events)if(subset(x,e.first))expected+=e.second;if(observed[i]!=expected)throw std::runtime_error("direct query mismatch");}
        std::cout<<"{\"status\":\"COMPLETE\",\"method\":\""<<method<<"\",\"query_count\":"<<count<<",\"objective_numerator\":\""<<pfl::score_to_string(checksum)<<"\",\"probability_denominator\":"<<a.mass<<",\"read_ms\":"<<read<<",\"preprocessing_ms\":"<<preprocessing<<",\"query_ms\":"<<query_ms<<",\"end_to_end_ms\":"<<preprocessing+query_ms<<",\"independent_verification_ms\":"<<ms(check_start)<<",\"peak_rss_bytes\":"<<rss<<",\"backend_core_bytes\":"<<b->bytes()<<",\"backend_states\":"<<b->states()<<",\"merge_calls\":"<<b->merges<<"}\n";
        return 0;
    }catch(const std::bad_alloc&){std::cout<<"{\"status\":\"MEMORY_LIMIT\"}\n";return 3;}
    catch(const std::exception& e){std::cout<<"{\"status\":\"ERROR\",\"message\":\""<<escape(e.what())<<"\"}\n";return 1;}
}
