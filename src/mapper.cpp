/* MIT License
 *
 * Copyright (c) 2018 Sam Kovaka <skovaka@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pdqsort.h"
#include "mapper.hpp"
#include "params.hpp"

u8 Mapper::PathBuffer::MAX_PATH_LEN = 0, 
   Mapper::PathBuffer::TYPE_MASK = 0;

u32 Mapper::PathBuffer::TYPE_ADDS[EventType::NUM_TYPES];

Mapper::PathBuffer::PathBuffer()
    : length_(0),
      prob_sums_(new float[MAX_PATH_LEN+1]) {
}

Mapper::PathBuffer::PathBuffer(const PathBuffer &p) {
    std::memcpy(this, &p, sizeof(PathBuffer));
}

void Mapper::PathBuffer::free_buffers() {
    delete[] prob_sums_;
}

void Mapper::PathBuffer::make_source(Range &range, u16 kmer, float prob) {
    length_ = 1;
    consec_stays_ = 0;
    event_types_ = 0;
    seed_prob_ = prob;
    fm_range_ = range;
    kmer_ = kmer;
    sa_checked_ = false;

    path_type_counts_[EventType::MATCH] = 1;
    path_type_counts_[EventType::STAY] = 0;
    total_match_len_ = 1;

    //TODO: don't write this here to speed up source loop
    prob_sums_[0] = 0;
    prob_sums_[1] = prob;

}


void Mapper::PathBuffer::make_child(PathBuffer &p, 
                                    Range &range,
                                    u16 kmer, 
                                    float prob, 
                                    EventType type) {

    length_ = p.length_ + (p.length_ <= MAX_PATH_LEN);
    fm_range_ = range;
    kmer_ = kmer;
    sa_checked_ = p.sa_checked_;
    event_types_ = TYPE_ADDS[type] | (p.event_types_ >> TYPE_BITS);
    consec_stays_ = (p.consec_stays_ + (type == EventType::STAY)) * (type == EventType::STAY);

    std::memcpy(path_type_counts_, p.path_type_counts_, EventType::NUM_TYPES * sizeof(u8));
    path_type_counts_[type]++;
    total_match_len_ = p.total_match_len_ + (type==EventType::MATCH);

    if (length_ > MAX_PATH_LEN) {
        std::memcpy(prob_sums_, &(p.prob_sums_[1]), MAX_PATH_LEN * sizeof(float));
        prob_sums_[MAX_PATH_LEN] = prob_sums_[MAX_PATH_LEN-1] + prob;
        seed_prob_ = (prob_sums_[MAX_PATH_LEN] - prob_sums_[0]) / MAX_PATH_LEN;
        path_type_counts_[p.type_tail()]--;
    } else {
        std::memcpy(prob_sums_, p.prob_sums_, length_ * sizeof(float));
        prob_sums_[length_] = prob_sums_[length_-1] + prob;
        seed_prob_ = prob_sums_[length_] / length_;
    }

}

void Mapper::PathBuffer::invalidate() {
    length_ = 0;
}

bool Mapper::PathBuffer::is_valid() const {
    return length_ > 0;
}

u8 Mapper::PathBuffer::match_len() const {
    return path_type_counts_[EventType::MATCH];
}

u8 Mapper::PathBuffer::type_head() const {
    return (event_types_ >> (TYPE_BITS*(MAX_PATH_LEN-2))) & TYPE_MASK;
}

u8 Mapper::PathBuffer::type_tail() const {
    return event_types_ & TYPE_MASK;
}

bool Mapper::PathBuffer::is_seed_valid(bool path_ended) const{
    return (fm_range_.length() == 1 || 
                (path_ended &&
                 fm_range_.length() <= PARAMS.max_rep_copy &&
                 match_len() >= PARAMS.min_rep_len)) &&

           length_ >= PARAMS.seed_len &&
           (path_ended || type_head() == EventType::MATCH) &&
           (path_ended || path_type_counts_[EventType::STAY] <= PARAMS.max_stay_frac * PARAMS.seed_len) &&
          seed_prob_ >= PARAMS.min_seed_prob;
}


bool operator< (const Mapper::PathBuffer &p1, 
                const Mapper::PathBuffer &p2) {
    return p1.fm_range_ < p2.fm_range_ ||
           (p1.fm_range_ == p2.fm_range_ && 
            p1.seed_prob_ < p2.seed_prob_);
}

Mapper::Mapper()
    : state_(State::INACTIVE) {


    PathBuffer::MAX_PATH_LEN = PARAMS.seed_len;

    for (u64 t = 0; t < EventType::NUM_TYPES; t++) {
        PathBuffer::TYPE_ADDS[t] = t << ((PathBuffer::MAX_PATH_LEN-2)*TYPE_BITS);
    }
    PathBuffer::TYPE_MASK = (u8) ((1 << TYPE_BITS) - 1);

    kmer_probs_ = std::vector<float>(PARAMS.model.kmer_count());
    prev_paths_ = std::vector<PathBuffer>(PARAMS.max_paths);
    next_paths_ = std::vector<PathBuffer>(PARAMS.max_paths);
    sources_added_ = std::vector<bool>(PARAMS.model.kmer_count(), false);

    prev_size_ = 0;
    event_i_ = 0;
    seed_tracker_.reset();
}

Mapper::Mapper(const Mapper &m) : Mapper() {}

Mapper::~Mapper() {

    for (u32 i = 0; i < next_paths_.size(); i++) {
        next_paths_[i].free_buffers();
        prev_paths_[i].free_buffers();
    }
}

ReadBuffer &Mapper::get_read() {
    return read_;
}

void Mapper::deactivate() {
    state_ = State::INACTIVE;
    reset_ = false;
}

Paf Mapper::map_read() {
    if (read_.loc_.is_mapped()) return read_.loc_;
    
    map_timer_.reset();

    std::vector<Event> events = event_detector_.add_samples(read_.full_signal_);
    PARAMS.model.normalize(events);
     
    for (u32 e = 0; e < events.size(); e++) {
        if (add_event(events[e].mean)) break;
    }

    read_.loc_.set_float(Paf::Tag::MAP_TIME, map_timer_.get());

    //if (!read_.loc_.is_mapped()) {
    //    read_.loc_.set_float(Paf::Tag::TOP_RATIO, seed_tracker_.get_top_conf());
    //    read_.loc_.set_float(Paf::Tag::MEAN_RATIO, seed_tracker_.get_mean_conf());
    //    set_ref_loc(seed_tracker_.get_best());
    //    //read_.loc_.print_paf();
    //    //seed_tracker_.print(std::cout, 5);
    //}

    return read_.loc_;
}

void Mapper::new_read(ReadBuffer &r) {
    read_.clear();//TODO: probably shouldn't auto erase previous read
    read_.swap(r);
    reset();

    #ifdef DEBUG_SEEDS
    seeds_out_.open(read_.id_ + "_seeds.bed");
    #endif

}

void Mapper::new_read(Chunk &chunk) {
    if (prev_unfinished(chunk.get_number())) {
        std::cerr << "Error: possibly lost read '" << read_.id_ << "'\n";
    }
    read_ = ReadBuffer(chunk);
    reset();
}

void Mapper::reset() {
    #ifdef DEBUG_SEEDS
    seeds_out_.close();
    #endif

    prev_size_ = 0;
    event_i_ = 0;
    reset_ = false;
    last_chunk_ = false;
    state_ = State::MAPPING;
    norm_.skip_unread();

    seed_tracker_.reset();
    event_detector_.reset();

    chunk_timer_.reset();
    map_timer_.reset();
    map_time_ = 0;
    wait_time_ = 0;
}

u32 Mapper::prev_unfinished(u32 next_number) const {
    return state_ == State::MAPPING && read_.number_ != next_number;
}

bool Mapper::finished() const {
    return state_ == State::SUCCESS || state_ == State::FAILURE;
}

void Mapper::skip_events(u32 n) {
    event_i_ += n;
    prev_size_ = 0;
}

void Mapper::request_reset() {
    reset_ = true;
}

void Mapper::end_reset() {
    reset_ = false;
}

bool Mapper::is_resetting() {
    return reset_;
}

bool Mapper::is_chunk_processed() const {
    return read_.chunk_processed_;
}

Mapper::State Mapper::get_state() const {
    return state_;
}

bool Mapper::add_chunk(Chunk &chunk) {
    if (!is_chunk_processed() || reset_) return false;

    if (read_.num_chunks_ == PARAMS.max_chunks_proc) {
        set_failed();
        chunk.clear();
        return true;
    }

    bool added = read_.add_chunk(chunk);
    chunk_timer_.reset();
    if (!added) std::cout << "# NOT ADDED " << chunk.get_id() << "\n";
    return added;
}

u16 Mapper::process_chunk() {
    if (read_.chunk_processed_ || reset_) return 0; 

    wait_time_ += map_timer_.lap();

    float mean;

    u16 nevents = 0;
    for (u32 i = 0; i < read_.chunk_.size(); i++) {
        if (event_detector_.add_sample(read_.chunk_[i])) {
            mean = event_detector_.get_mean();
            if (!norm_.add_event(mean)) {

                u32 nskip = norm_.skip_unread(nevents);
                skip_events(nskip);
                if (!norm_.add_event(mean)) {
                    map_time_ += map_timer_.lap();
                    return nevents;
                }
            }
            nevents++;
        }
    }

    read_.chunk_.clear();
    read_.chunk_processed_ = true;

    map_time_ += map_timer_.lap();

    return nevents;
}

void Mapper::set_failed() {
    state_ = State::FAILURE;
    reset_ = false;

    read_.loc_.set_float(Paf::Tag::MAP_TIME, map_time_);
    read_.loc_.set_float(Paf::Tag::WAIT_TIME, wait_time_);
}


bool Mapper::map_chunk() {
    wait_time_ += map_timer_.lap();

    if (reset_ || chunk_timer_.get() > PARAMS.max_chunk_wait) {
        set_failed();
        read_.loc_.set_ended();
        return true;

    } else if (norm_.empty() && 
               read_.chunk_processed_ && 
               read_.num_chunks_ == PARAMS.max_chunks_proc) {
        set_failed();
        return true;

    } else if (norm_.empty()) {
        return false;
    }

    u16 nevents = PARAMS.get_max_events(event_i_);
    float tlimit = PARAMS.evt_timeout * nevents;

    for (u16 i = 0; i < nevents && !norm_.empty(); i++) {
        if (add_event(norm_.pop_event())) {
            read_.loc_.set_float(Paf::Tag::MAP_TIME, map_time_+map_timer_.get());
            read_.loc_.set_float(Paf::Tag::WAIT_TIME, wait_time_);
            return true;
        }
        if (map_timer_.get() > tlimit) break;
    }

    map_time_ += map_timer_.lap();

    return false;
}

bool Mapper::add_event(float event) {

    if (reset_ || event_i_ >= PARAMS.max_events_proc) {
        state_ = State::FAILURE;
        return true;
    }

    Range prev_range;
    u16 prev_kmer;
    float evpr_thresh;
    bool child_found;


    auto next_path = next_paths_.begin();

    for (u16 kmer = 0; kmer < PARAMS.model.kmer_count(); kmer++) {
        kmer_probs_[kmer] = PARAMS.model.event_match_prob(event, kmer);
    }
    
    //Find neighbors of previous nodes
    for (u32 pi = 0; pi < prev_size_; pi++) {
        if (!prev_paths_[pi].is_valid()) {
            continue;
        }

        child_found = false;

        PathBuffer &prev_path = prev_paths_[pi];
        Range &prev_range = prev_path.fm_range_;
        prev_kmer = prev_path.kmer_;

        evpr_thresh = PARAMS.get_prob_thresh(prev_range.length());
        //evpr_thresh = PARAMS.get_path_thresh(prev_path.total_match_len_);

        if (prev_path.consec_stays_ < PARAMS.max_consec_stay && 
            kmer_probs_[prev_kmer] >= evpr_thresh) {

            next_path->make_child(prev_path, 
                                  prev_range,
                                  prev_kmer, 
                                  kmer_probs_[prev_kmer], 
                                  EventType::STAY);
            #ifdef FM_PROFILER
            fm_profiler_.add_range(prev_range);
            #endif

            child_found = true;

            if (++next_path == next_paths_.end()) {
                break;
            }
        }

        //Add all the neighbors
        for (u8 b = 0; b < ALPH_SIZE; b++) {
            u16 next_kmer = PARAMS.model.get_neighbor(prev_kmer, b);

            if (kmer_probs_[next_kmer] < evpr_thresh) {
                continue;
            }

            Range next_range = PARAMS.fmi.get_neighbor(prev_range, b);

            if (!next_range.is_valid()) {
                continue;
            }

            next_path->make_child(prev_path, 
                                  next_range,
                                  next_kmer, 
                                  kmer_probs_[next_kmer], 
                                  EventType::MATCH);


            #ifdef FM_PROFILER
            fm_profiler_.add_range(next_range);
            #endif

            child_found = true;

            if (++next_path == next_paths_.end()) {
                break;
            }
        }


        if (!child_found && !prev_path.sa_checked_) {

            //Add seeds for non-extended paths
            //Extended paths will be updated after sources filled in
            update_seeds(prev_path, true);

            #ifdef DEBUG_SEEDS
            print_debug_seeds(prev_path);
            #endif
        }

        if (next_path == next_paths_.end()) {
            break;
        }
    }

    //Create sources between gaps
    if (next_path != next_paths_.begin()) {

        u32 next_size = next_path - next_paths_.begin();

        pdqsort(next_paths_.begin(), next_path);
        //std::sort(next_paths_.begin(), next_path);

        u16 source_kmer;
        prev_kmer = PARAMS.model.kmer_count(); 

        Range unchecked_range, source_range;

        for (u32 i = 0; i < next_size; i++) {
            source_kmer = next_paths_[i].kmer_;

            //Add source for beginning of kmer range
            if (source_kmer != prev_kmer &&
                next_path != next_paths_.end() &&
                kmer_probs_[source_kmer] >= PARAMS.get_source_prob()) {

                sources_added_[source_kmer] = true;

                source_range = Range(PARAMS.kmer_fmranges[source_kmer].start_,
                                     next_paths_[i].fm_range_.start_ - 1);

                if (source_range.is_valid()) {
                    next_path->make_source(source_range,
                                           source_kmer,
                                           kmer_probs_[source_kmer]);
                    next_path++;

                    #ifdef FM_PROFILER
                    fm_profiler_.add_range(source_range);
                    #endif
                }                                    

                unchecked_range = Range(next_paths_[i].fm_range_.end_ + 1,
                                        PARAMS.kmer_fmranges[source_kmer].end_);
            }

            prev_kmer = source_kmer;

            //Range next_range = next_paths_[i].fm_range_;

            //Remove paths with duplicate ranges
            //Best path will be listed last
            if (i < next_size - 1 && next_paths_[i].fm_range_ == next_paths_[i+1].fm_range_) {
                next_paths_[i].invalidate();
                continue;
            }

            //Start source after current path
            //TODO: check if theres space for a source here, instead of after extra work?
            if (next_path != next_paths_.end() &&
                kmer_probs_[source_kmer] >= PARAMS.get_source_prob()) {
                
                source_range = unchecked_range;
                
                //Between this and next path ranges
                if (i < next_size - 1 && source_kmer == next_paths_[i+1].kmer_) {

                    source_range.end_ = next_paths_[i+1].fm_range_.start_ - 1;

                    if (unchecked_range.start_ <= next_paths_[i+1].fm_range_.end_) {
                        unchecked_range.start_ = next_paths_[i+1].fm_range_.end_ + 1;
                    }
                }

                //Add it if it's a real range
                if (source_range.is_valid()) {

                    next_path->make_source(source_range,
                                           source_kmer,
                                           kmer_probs_[source_kmer]);
                    next_path++;

                    #ifdef FM_PROFILER
                    fm_profiler_.add_range(source_range);
                    #endif
                }
            }

            update_seeds(next_paths_[i], false);
        }
    }

    for (u16 kmer = 0; 
         kmer < PARAMS.model.kmer_count() && 
            next_path != next_paths_.end(); 
         kmer++) {

        Range next_range = PARAMS.kmer_fmranges[kmer];

        if (!sources_added_[kmer] && 
            kmer_probs_[kmer] >= PARAMS.get_source_prob() &&
            next_path != next_paths_.end() &&
            next_range.is_valid()) {

            //TODO: don't write to prob buffer here to speed up source loop
            next_path->make_source(next_range, kmer, kmer_probs_[kmer]);
            next_path++;

            #ifdef FM_PROFILER
            fm_profiler_.add_kmer(kmer);
            #endif

        } else {
            sources_added_[kmer] = false;
        }
    }

    prev_size_ = next_path - next_paths_.begin();
    prev_paths_.swap(next_paths_);

    //Update event index
    event_i_++;

    SeedGroup sg = seed_tracker_.get_final();

    if (sg.is_valid()) {
        state_ = State::SUCCESS;
        set_ref_loc(sg);

        #ifdef DEBUG_SEEDS
        for (u32 pi = 0; pi < prev_size_; pi++) {
            print_debug_seeds(prev_paths_[pi]);
        }
        #endif

        return true;
    }

    return false;
}

void Mapper::update_seeds(PathBuffer &p, bool path_ended) {

    if (p.is_seed_valid(path_ended)) {

        p.sa_checked_ = true;

        for (u64 s = p.fm_range_.start_; s <= p.fm_range_.end_; s++) {

            //Reverse the reference coords so they both go L->R
            u64 ref_en = PARAMS.fmi.size() - PARAMS.fmi.sa(s) + 1;

            seed_tracker_.add_seed(ref_en, p.match_len(), event_i_ - path_ended);
        }
    }
}

#ifdef DEBUG_SEEDS
void Mapper::print_debug_seeds(PathBuffer &p) {

    if (!p.is_seed_valid(true)) return;

    for (u64 s = p.fm_range_.start_; s <= p.fm_range_.end_; s++) {
        //Reverse the reference coords so they both go L->R
        u64 ref_en = PARAMS.fmi.size() - (PARAMS.fmi.sa(s) + 1);

        bool fwd = ref_en < PARAMS.fmi.size() / 2;

        u64 sa_st;
        if (fwd) sa_st = ref_en - (p.match_len() + PARAMS.model.kmer_len() - 1)  + 1;
        else     sa_st = PARAMS.fmi.size() - ref_en - 1;

        std::string rf_name;
        u64 rf_st;
        PARAMS.fmi.translate_loc(sa_st, rf_name, rf_st);

        if (rf_st > PARAMS.fmi.size()) {
            rf_st = 0;
        }
          
        seeds_out_ << rf_name << "\t"
                   << rf_st << "\t"
                   << (rf_st + p.match_len() + PARAMS.model.kmer_len() - 1) << "\t"
                   << event_i_ << "\t"
                   << (fwd ? "+" : "-") << "\n";
    }
}
#endif

void Mapper::set_ref_loc(const SeedGroup &seeds) {
    bool fwd = seeds.ref_st_ < PARAMS.fmi.size() / 2;

    u64 sa_st;
    if (fwd) sa_st = seeds.ref_st_;
    else      sa_st = PARAMS.fmi.size() - (seeds.ref_en_.end_ + PARAMS.model.kmer_len() - 1);
    
    std::string rf_name;
    u64 rd_st = event_detector_.event_to_bp(seeds.evt_st_),
        rd_en = event_detector_.event_to_bp(seeds.evt_en_ + PARAMS.seed_len, true),
        rf_st,
        rf_len = PARAMS.fmi.translate_loc(sa_st, rf_name, rf_st), //sets rf_st
        rf_en = rf_st + (seeds.ref_en_.end_ - seeds.ref_st_ + PARAMS.model.kmer_len());

    u16 match_count = seeds.total_len_ + PARAMS.model.kmer_len() - 1;

    read_.loc_.set_mapped(rd_st, rd_en, rf_name, rf_st, rf_en, rf_len, fwd, match_count);
}


