/**
 * Copyright 2018 VMware
 * Copyright 2018 Ted Yin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hotstuff/hotstuff.h"
#include "hotstuff/client.h"
#include "hotstuff/liveness.h"

using salticidae::static_pointer_cast;

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN

namespace hotstuff {

const opcode_t MsgProof::opcode;
MsgProof::MsgProof(const Proof &proof) { serialized << proof; }
void MsgCommitted::postponed_parse(HotStuffCore *hsc) {
    proof.hsc = hsc;
    serialized >> chain;
}

const opcode_t MsgCommitted::opcode;
MsgCommitted::MsgCommitted(const ChainCommitted &chain) { serialized << chain; }
void MsgCommitted::postponed_parse(HotStuffCore *hsc) {
    chain.hsc = hsc;
    serialized >> chain;
}

const opcode_t MsgPropose::opcode;
MsgPropose::MsgPropose(const Proposal &proposal) { serialized << proposal; }
void MsgPropose::postponed_parse(HotStuffCore *hsc) {
    proposal.hsc = hsc;
    serialized >> proposal;
}

const opcode_t MsgVote::opcode;
MsgVote::MsgVote(const Vote &vote) { serialized << vote; }
void MsgVote::postponed_parse(HotStuffCore *hsc) {
    vote.hsc = hsc;
    serialized >> vote;
}

const opcode_t MsgReqBlock::opcode;
MsgReqBlock::MsgReqBlock(const std::vector<uint256_t> &blk_hashes) {
    serialized << htole((uint32_t)blk_hashes.size());
    for (const auto &h: blk_hashes)
        serialized << h;
}

MsgReqBlock::MsgReqBlock(DataStream &&s) {
    uint32_t size;
    s >> size;
    size = letoh(size);
    blk_hashes.resize(size);
    for (auto &h: blk_hashes) s >> h;
}

const opcode_t MsgRespBlock::opcode;
MsgRespBlock::MsgRespBlock(const std::vector<block_t> &blks) {
    serialized << htole((uint32_t)blks.size());
    for (auto blk: blks) serialized << *blk;
}

void MsgRespBlock::postponed_parse(HotStuffCore *hsc) {
    uint32_t size;
    serialized >> size;
    size = letoh(size);
    blks.resize(size);
    for (auto &blk: blks)
    {
        Block _blk;
        _blk.unserialize(serialized, hsc);
        blk = hsc->storage->add_blk(std::move(_blk), hsc->get_config());
    }
}

// TODO: improve this function
void HotStuffBase::exec_command(uint256_t cmd_hash, commit_cb_t callback) {
    cmd_pending.enqueue(std::make_pair(cmd_hash, callback));
}

void HotStuffBase::on_fetch_blk(const block_t &blk) {
#ifdef HOTSTUFF_BLK_PROFILE
    blk_profiler.get_tx(blk->get_hash());
#endif
    LOG_DEBUG("fetched %.10s", get_hex(blk->get_hash()).c_str());
    part_fetched++;
    fetched++;
    //for (auto cmd: blk->get_cmds()) on_fetch_cmd(cmd);
    const uint256_t &blk_hash = blk->get_hash();
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it != blk_fetch_waiting.end())
    {
        it->second.resolve(blk);
        blk_fetch_waiting.erase(it);
    }
}

bool HotStuffBase::on_deliver_blk(const block_t &blk) {
    const uint256_t &blk_hash = blk->get_hash();
    bool valid;
    /* sanity check: all parents must be delivered */
    for (const auto &p: blk->get_parent_hashes())
        assert(storage->is_blk_delivered(p));
    if ((valid = HotStuffCore::on_deliver_blk(blk)))
    {
        LOG_DEBUG("block %.10s delivered",
                get_hex(blk_hash).c_str());
        part_parent_size += blk->get_parent_hashes().size();
        part_delivered++;
        delivered++;
    }
    else
    {
        LOG_WARN("dropping invalid block");
    }

    bool res = true;
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
    {
        auto &pm = it->second;
        if (valid)
        {
            pm.elapsed.stop(false);
            auto sec = pm.elapsed.elapsed_sec;
            part_delivery_time += sec;
            part_delivery_time_min = std::min(part_delivery_time_min, sec);
            part_delivery_time_max = std::max(part_delivery_time_max, sec);

            pm.resolve(blk);
        }
        else
        {
            pm.reject(blk);
            res = false;
            // TODO: do we need to also free it from storage?
        }
        blk_delivery_waiting.erase(it);
    }
    return res;
}

promise_t HotStuffBase::async_fetch_blk(const uint256_t &blk_hash,
                                        const PeerId *replica,
                                        bool fetch_now) {
    if (storage->is_blk_fetched(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm){
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it == blk_fetch_waiting.end())
    {
#ifdef HOTSTUFF_BLK_PROFILE
        blk_profiler.rec_tx(blk_hash, false);
#endif
        it = blk_fetch_waiting.insert(
            std::make_pair(
                blk_hash,
                BlockFetchContext(blk_hash, this))).first;
    }
    if (replica != nullptr)
        it->second.add_replica(*replica, fetch_now);
    return static_cast<promise_t &>(it->second);
}

promise_t HotStuffBase::async_deliver_blk(const uint256_t &blk_hash,
                                        const PeerId &replica) {
    if (storage->is_blk_delivered(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm) {
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
        return static_cast<promise_t &>(it->second);
    BlockDeliveryContext pm{[](promise_t){}};
    it = blk_delivery_waiting.insert(std::make_pair(blk_hash, pm)).first;
    /* otherwise the on_deliver_batch will resolve */
    async_fetch_blk(blk_hash, &replica).then([this, replica](block_t blk) {
        /* qc_ref should be fetched */
        std::vector<promise_t> pms;
        const auto &qc = blk->get_qc();
        assert(qc);
        if (blk == get_genesis())
            pms.push_back(promise_t([](promise_t &pm){ pm.resolve(true); }));
        else
            pms.push_back(blk->verify(this, vpool));
        pms.push_back(async_fetch_blk(qc->get_obj_hash(), &replica));
        /* the parents should be delivered */
        for (const auto &phash: blk->get_parent_hashes())
            pms.push_back(async_deliver_blk(phash, replica));
        promise::all(pms).then([this, blk](const promise::values_t values) {
            auto ret = promise::any_cast<bool>(values[0]) && this->on_deliver_blk(blk);
            if (!ret)
                HOTSTUFF_LOG_WARN("verification failed during async delivery");
        });
    });
    return static_cast<promise_t &>(pm);
}

bool HotStuffBase::conflicting(const block_t &blkA, const block_t &blkB){
    return ((blkA->get_height() == blkB->get_height()) && (blkA->get_hash() != blkB->get_hash()));
}

bool HotStuffBase::check_lastBlockChain(const block_t &blk1, const block_t &blk2){
    if(blk1->get_height() == blk2->get_height() && blk1->get_qc_ref() == blk2->get_qc_ref() && blk1->get_qc() == blk2->get_qc()){
        return true;
    }
    else{
        return false;
    }
}

bool HotStuffBase::invalid_unlocking(const block_t &blkA, const block_t &blkB){
    bool invalid = false;
    std::vector<block_t> parentsA = blkA->get_parents();
    //if((check_lastBlockChain(blkB, get_blk2())) && (blkA->get_height() > blkB->get_height()) && (parentsA[0]->get_height() < ((blkB->get_height()) - 2))){
    if((blkA->get_height() > blkB->get_height()) && (parentsA[0]->get_height() < ((blkB->get_height()) - 2))){   
        invalid = true;
        return invalid;
    }
    else {
        return invalid;
    }
}

void HotStuffBase::committed_handler(MsgCommitted &&msg, const Net::conn_t &conn) {
    LOG_INFO("Committed message received!");

    const PeerId &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);

    //Save the blks received through the MsgCommitted into a global variable 
    auto &chain_ = msg.chain;
    block_t blk = chain_.blk;
    if (!blk) return;
    block_t blk1 = chain_.blk1;
    if (!blk1) return;
    block_t blk2 = chain_.blk2;
    if (!blk2) return;

    // CHECK WHETHER THE RECEIVED BLOCKS CONSTITUTE A THREE-CHAIN
    // Check whether the received blocks indeed form a three-chain
    if !((blk2->get_height() == blk1->get_height() + 1) && (blk1->get_height() == blk->get_height() + 1)) return;

    // Sanity check
    if !((blk2->parents[0] == blk1) && (blk1->parents[0] == blk)) return;

    blks_received.insert(std::make_pair(blk->get_hash(), blk));
    blks_received.insert(std::make_pair(blk1->get_hash(), blk1));
    blks_received.insert(std::make_pair(blk2->get_hash(), blk2));
    
    // std::unordered_map<const uint256_t, block_t> blks_received_local;
    // blks_received_local.insert(std::make_pair(blk->get_hash(), blk));
    // blks_received_local.insert(std::make_pair(blk1->get_hash(), blk1));
    // blks_received_local.insert(std::make_pair(blk2->get_hash(), blk2));

    LOG_INFO("blks_received: %lu", get_blks_received_size()); 
    LOG_INFO("blk_cache: %lu", storage->get_blk_cache_size());
    periodicalCheck_conflicting();
    //periodicalCheck_invalid_unlocking(storage->get_blk_cache(), blks_received);
    
    auto search = blks_received.find(blk2->get_hash());
    if(search != blks_received.end()){
        periodicalCheck_invalid_unlocking(storage->get_blk_cache(), search->second);
    }
    else{
         LOG_INFO("Not found");
    }
}

void HotStuffBase::proof_handler(MsgProof &&msg, const Net::conn_t &conn) {
    LOG_INFO("Proof message received!");

    const PeerId &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);

    auto &proof_ = msg.proof;
    block_t blk1_conflict = proof_.blk1_conflict;
    if (!blk1_conflict) return;
    block_t blk2_conflict = proof_.blk2_conflict;
    if (!blk2_conflict) return;

    auto voted_blk1 = blk1_conflict->get_voted();
    auto voted_blk2 = blk2_conflict->get_voted();
    for(auto it_1 = voted_blk1.begin(); it_1 != voted_blk1.end(); it_1++){
        for(auto it_2 = voted_blk2.begin(); it_2 != voted_blk2.end(); it_2++){
            if (*it_1 == *it_2)
                {
                    LOG_WARN("Faulty replica: %s", std::to_string(*it_1));
                }
        }
    }
}

void HotStuffBase::periodicalCheck_conflicting() {
    auto blk_cache = storage->get_blk_cache();
    for(auto &i : blk_cache){
        for(auto &j : blks_received){
            if(conflicting(i.second,j.second)){
                LOG_WARN("Found a conflict!");
                // another type of message we call PROOF
                // put both blocks and send the message to everyone and that's it!
                Proof proof(i.second,j.second,this);
                on_receive_proof(proof);
                /* broadcast to all replicas */
                do_broadcast_proof(proof);
            else{
                LOG_INFO("Everything is fine!");
            }
        }
    }
}

void HotStuffBase::periodicalCheck_invalid_unlocking(const std::unordered_map<const uint256_t, block_t> &blk_cache, const block_t &blk2){
    for(auto &i : blk_cache){
        if(invalid_unlocking(i.second, blk2)){
            LOG_WARN("Found an invalid unlocking!");
            //calculate the proof of culpability 
            LOG_WARN("Found a conflict!");
            // another type of message we call PROOF
            // put both blocks and send the message to everyone and that's it!
            Proof proof(i.second,blk2,this);
            on_receive_proof(proof);
            /* broadcast to all replicas */
            do_broadcast_proof(proof);
        }
        else{
            LOG_INFO("Everything is fine!");
        }
    }
}



// void HotStuffBase::periodicalCheck_invalid_unlocking(const std::unordered_map<const uint256_t, block_t> &blk_cache, const std::unordered_map<const uint256_t, block_t> &blks_rec){
//     for(auto &i : blk_cache){
//         for(auto &j : blks_rec){
//             if(invalid_unlocking(i.second, j.second)){
//                 LOG_WARN("Find an invalid unlocking!");
//                 //calculate the proof of culpability 
//                 block_t blk_i = i.second;
//                 block_t blk_j = j.second;
//                 std::unordered_set<ReplicaID> voted_i = blk_i-> get_voted();
//                 std::unordered_set<ReplicaID> voted_j = blk_j-> get_voted();
//                 for(auto it_i = voted_i.begin(); it_i != voted_i.end(); it_i++){
//                     for(auto it_j = voted_j.begin(); it_j != voted_j.end(); it_j++){
//                         if (*it_i == *it_j)
//                         {
//                             LOG_WARN("Faulty replica: %s", std::to_string(*it_i));
//                         }
//                     }
//                 }
//                 return;
//             }
//             else{
//                 LOG_INFO("Everything is fine!");
//             }
//         }
//     }
// }


void HotStuffBase::propose_handler(MsgPropose &&msg, const Net::conn_t &conn) {
    const PeerId &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    auto &prop = msg.proposal;
    block_t blk = prop.blk;
    if (!blk) return;
    if (peer != get_config().get_peer_id(prop.proposer))
    {
        LOG_WARN("invalid proposal from %d", prop.proposer);
        return;
    }
    promise::all(std::vector<promise_t>{
        async_deliver_blk(blk->get_hash(), peer)
    }).then([this, prop = std::move(prop)]() {
        on_receive_proposal(prop);
    });
}

void HotStuffBase::vote_handler(MsgVote &&msg, const Net::conn_t &conn) {
    const auto &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    //auto &vote = msg.vote;
    RcObj<Vote> v(new Vote(std::move(msg.vote)));
    promise::all(std::vector<promise_t>{
        async_deliver_blk(v->blk_hash, peer),
        v->verify(vpool),
    }).then([this, v=std::move(v)](const promise::values_t values) {
        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN("invalid vote from %d", v->voter);
        else
            on_receive_vote(*v);
    });
}

void HotStuffBase::req_blk_handler(MsgReqBlock &&msg, const Net::conn_t &conn) {
    const PeerId replica = conn->get_peer_id();
    if (replica.is_null()) return;
    auto &blk_hashes = msg.blk_hashes;
    std::vector<promise_t> pms;
    for (const auto &h: blk_hashes)
        pms.push_back(async_fetch_blk(h, nullptr));
    promise::all(pms).then([replica, this](const promise::values_t values) {
        std::vector<block_t> blks;
        for (auto &v: values)
        {
            auto blk = promise::any_cast<block_t>(v);
            blks.push_back(blk);
        }
        pn.send_msg(MsgRespBlock(blks), replica);
    });
}

void HotStuffBase::resp_blk_handler(MsgRespBlock &&msg, const Net::conn_t &) {
    msg.postponed_parse(this);
    for (const auto &blk: msg.blks)
        if (blk) on_fetch_blk(blk);
}

bool HotStuffBase::conn_handler(const salticidae::ConnPool::conn_t &conn, bool connected) {
    if (connected)
    {
        if (!pn.enable_tls) return true;
        auto cert = conn->get_peer_cert();
        //SALTICIDAE_LOG_INFO("%s", salticidae::get_hash(cert->get_der()).to_hex().c_str());
        return valid_tls_certs.count(salticidae::get_hash(cert->get_der()));
    }
    return true;
}

void HotStuffBase::print_stat() const {
    LOG_INFO("===== begin stats =====");
    LOG_INFO("-------- queues -------");
    LOG_INFO("blk_fetch_waiting: %lu", blk_fetch_waiting.size());
    LOG_INFO("blk_delivery_waiting: %lu", blk_delivery_waiting.size());
    LOG_INFO("decision_waiting: %lu", decision_waiting.size());
    LOG_INFO("-------- misc ---------");
    LOG_INFO("fetched: %lu", fetched);
    LOG_INFO("delivered: %lu", delivered);
    LOG_INFO("cmd_cache: %lu", storage->get_cmd_cache_size());
    LOG_INFO("blk_cache: %lu", storage->get_blk_cache_size());
    LOG_INFO("------ misc (10s) -----");
    LOG_INFO("fetched: %lu", part_fetched);
    LOG_INFO("delivered: %lu", part_delivered);
    LOG_INFO("decided: %lu", part_decided);
    LOG_INFO("gened: %lu", part_gened);
    LOG_INFO("avg. parent_size: %.3f",
            part_delivered ? part_parent_size / double(part_delivered) : 0);
    LOG_INFO("delivery time: %.3f avg, %.3f min, %.3f max",
            part_delivered ? part_delivery_time / double(part_delivered) : 0,
            part_delivery_time_min == double_inf ? 0 : part_delivery_time_min,
            part_delivery_time_max);

    part_parent_size = 0;
    part_fetched = 0;
    part_delivered = 0;
    part_decided = 0;
    part_gened = 0;
    part_delivery_time = 0;
    part_delivery_time_min = double_inf;
    part_delivery_time_max = 0;
#ifdef HOTSTUFF_MSG_STAT
    LOG_INFO("--- replica msg. (10s) ---");
    size_t _nsent = 0;
    size_t _nrecv = 0;
    for (const auto &replica: peers)
    {
        auto conn = pn.get_peer_conn(replica);
        if (conn == nullptr) continue;
        size_t ns = conn->get_nsent();
        size_t nr = conn->get_nrecv();
        size_t nsb = conn->get_nsentb();
        size_t nrb = conn->get_nrecvb();
        conn->clear_msgstat();
        LOG_INFO("%s: %u(%u), %u(%u), %u",
            get_hex10(replica).c_str(), ns, nsb, nr, nrb, part_fetched_replica[replica]);
        _nsent += ns;
        _nrecv += nr;
        part_fetched_replica[replica] = 0;
    }
    nsent += _nsent;
    nrecv += _nrecv;
    LOG_INFO("sent: %lu", _nsent);
    LOG_INFO("recv: %lu", _nrecv);
    LOG_INFO("--- replica msg. total ---");
    LOG_INFO("sent: %lu", nsent);
    LOG_INFO("recv: %lu", nrecv);
#endif
    LOG_INFO("====== end stats ======");
}

HotStuffBase::HotStuffBase(uint32_t blk_size,
                    ReplicaID rid,
                    privkey_bt &&priv_key,
                    NetAddr listen_addr,
                    pacemaker_bt pmaker,
                    EventContext ec,
                    size_t nworker,
                    const Net::Config &netconfig):
        HotStuffCore(rid, std::move(priv_key)),
        listen_addr(listen_addr),
        blk_size(blk_size),
        ec(ec),
        tcall(ec),
        vpool(ec, nworker),
        pn(ec, netconfig),
        pmaker(std::move(pmaker)),
        fetched(0), delivered(0),
        nsent(0), nrecv(0),
        part_parent_size(0),
        part_fetched(0),
        part_delivered(0),
        part_decided(0),
        part_gened(0),
        part_delivery_time(0),
        part_delivery_time_min(double_inf),
        part_delivery_time_max(0)
{
    /* register the handlers for msg from replicas */
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::proof_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::committed_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::propose_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::vote_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::req_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::resp_blk_handler, this, _1, _2));
    pn.reg_conn_handler(salticidae::generic_bind(&HotStuffBase::conn_handler, this, _1, _2));
    pn.reg_error_handler([](const std::exception_ptr _err, bool fatal, int32_t async_id) {
        try {
            std::rethrow_exception(_err);
        } catch (const std::exception &err) {
            HOTSTUFF_LOG_WARN("network async error: %s\n", err.what());
        }
    });
    pn.start();
    pn.listen(listen_addr);
}

void HotStuffBase::do_broadcast_committed(const ChainCommitted &chain) {
    pn.multicast_msg(MsgCommitted(chain), peers);
}

void HotStuffBase::do_broadcast_proof(const Proof &proof) {
    pn.multicast_msg(MsgProof(proof), peers);
}

void HotStuffBase::do_broadcast_proposal(const Proposal &prop) {
    //MsgPropose prop_msg(prop);
    pn.multicast_msg(MsgPropose(prop), peers);
    //for (const auto &replica: peers)
    //    pn.send_msg(prop_msg, replica);
}

void HotStuffBase::do_vote(ReplicaID last_proposer, const Vote &vote) {
    pmaker->beat_resp(last_proposer)
            .then([this, vote](ReplicaID proposer) {
        if (proposer == get_id())
        {
            //throw HotStuffError("unreachable line");
            on_receive_vote(vote);
        }
        else
            pn.send_msg(MsgVote(vote), get_config().get_peer_id(proposer));
    });
}

void HotStuffBase::do_consensus(const block_t &blk) {
    pmaker->on_consensus(blk);
}

void HotStuffBase::do_decide(Finality &&fin) {
    part_decided++;
    state_machine_execute(fin);
    auto it = decision_waiting.find(fin.cmd_hash);
    if (it != decision_waiting.end())
    {
        it->second(std::move(fin));
        decision_waiting.erase(it);
    }
}

HotStuffBase::~HotStuffBase() {}

void HotStuffBase::start(
        std::vector<std::tuple<NetAddr, pubkey_bt, uint256_t>> &&replicas,
        bool ec_loop) {
    for (size_t i = 0; i < replicas.size(); i++)
    {
        auto &addr = std::get<0>(replicas[i]);
        auto cert_hash = std::move(std::get<2>(replicas[i]));
        valid_tls_certs.insert(cert_hash);
        auto peer = pn.enable_tls ? salticidae::PeerId(cert_hash) : salticidae::PeerId(addr);
        HotStuffCore::add_replica(i, peer, std::move(std::get<1>(replicas[i])));
        if (addr != listen_addr)
        {
            peers.push_back(peer);
            pn.add_peer(peer);
            pn.set_peer_addr(peer, addr);
            pn.conn_peer(peer);
        }
    }

    /* ((n - 1) + 1 - 1) / 3 */
    uint32_t nfaulty = peers.size() / 3;
    if (nfaulty == 0)
        LOG_WARN("too few replicas in the system to tolerate any failure");
    on_init(nfaulty);
    pmaker->init(this);
    if (ec_loop)
        ec.dispatch();

    cmd_pending.reg_handler(ec, [this](cmd_queue_t &q) {
        std::pair<uint256_t, commit_cb_t> e;
        while (q.try_dequeue(e))
        {
            ReplicaID proposer = pmaker->get_proposer();

            const auto &cmd_hash = e.first;
            auto it = decision_waiting.find(cmd_hash);
            if (it == decision_waiting.end())
                it = decision_waiting.insert(std::make_pair(cmd_hash, e.second)).first;
            else
                e.second(Finality(id, 0, 0, 0, cmd_hash, uint256_t()));
            if (proposer != get_id()) continue;
            cmd_pending_buffer.push(cmd_hash);
            if (cmd_pending_buffer.size() >= blk_size)
            {
                std::vector<uint256_t> cmds;
                for (uint32_t i = 0; i < blk_size; i++)
                {
                    cmds.push_back(cmd_pending_buffer.front());
                    cmd_pending_buffer.pop();
                }
                pmaker->beat().then([this, cmds = std::move(cmds)](ReplicaID proposer) {
                    if (proposer == get_id())
                        on_propose(cmds, pmaker->get_parents());
                });
                return true;
            }
        }
        return false;
    });
}

}
