#include "FloorManager.h"
#include "ExecutionReport.h"
#include "time_util.h"
#include <stdexcept>
#include <atomic>

namespace pm {

    FloorManager& FloorManager::get() {
        static FloorManager mgr("floor");
        return mgr;
    };

    FloorManager::~FloorManager() {};
    void FloorManager::start() {
        // process of startup
        // 1. create posisiotn manager and load eod
        // 2. create channel and start to process er
        // 3. send request for replay
        // 4. wait for done
        // 5. process recovery
        // 6. request open order
        // 7. Mark SoD load done
        // 8. wait for 2 seconds before accepting position requests
        // 9. enter the loop

        if (m_started) {
            fprintf(stderr, "Floor Manager already started!\n");
            return;
        }
        m_started = true;
        m_loaded = false;
        m_should_run = true;
        m_eod_pending = false;
        m_loaded_time = 0;
        std::string errstr;
        while (m_should_run && (!requestReplay(m_pm.getLoadUtc(), &errstr))) {
            fprintf(stderr, "problem requesting replay: %s, retrying in next 5 seconds\n", errstr.c_str());
            utils::TimeUtil::micro_sleep(5*1000000);
            continue;
        };
        while (m_should_run && (!requestOpenOrder(&errstr))) {
            fprintf(stderr, "problem requesting open orders download: %s, retrying in next 5 seconds\n", errstr.c_str());
            utils::TimeUtil::micro_sleep(5*1000000);
            continue;
        }

        setInitialSubscriptions();
        while (m_should_run) {
            bool has_message = this->run_one_loop(*this);
            if (! has_message) {
                // idle, don't spin
                // any other tasks could be performed here

                // check loaded time
                if ((!m_loaded) && (m_loaded_time > 0)) {
                    // wait for 2 second before subscribing position requests
                    if (time(nullptr) > m_loaded_time + 1 ) {
                        m_loaded = true;
                        addPositionSubscriptions();
                        fprintf(stderr, "%s accepting position requests\n", m_name.c_str());
                    }
                }
                utils::TimeUtil::micro_sleep(1000);
            }
        }
        fprintf(stderr, "Stop received, exit.\n");
        m_started = false;
        m_loaded = false;
    }

    FloorManager::FloorManager(const std::string& name)
    : FloorBase(name, true), 
      m_pm(m_name),
      m_started(false), m_loaded(false), m_should_run(false), m_eod_pending(false),
      m_loaded_time(0)
    {}

    void FloorManager::handleMessage(MsgType& msg_in) {
        switch (msg_in.type) {
        case ExecutionReport:
        {
            handleExecutionReport(msg_in);
            break;
        }
        case FloorBase::ExecutionReplayDone: 
        {
            if (!m_eod_pending) {
                // start up recovery
                m_pm.loadRecovery(m_recovery_file);
                fprintf(stderr, "recovery done!\n");
                m_loaded_time = time(nullptr);
            } else {
                std::string difflog;
                if (!m_pm.reconcile(m_recovery_file, difflog, false)) {
                    fprintf(stderr, "%s failed to reconcile! \nrecovery_file: %s\ndiff:%s\n", 
                            m_name.c_str(), m_recovery_file.c_str(), difflog.c_str());
                } else {
                    m_pm.persist();
                    fprintf(stderr, "%s EoD Done!\n", m_name.c_str());
                }
                m_eod_pending = false;
            }
            break;
        }
        case FloorBase::UserReq :
        {
            handleUserReq(msg_in);
            m_channel->update(m_msgout);
            break;
        }
        case FloorBase::GetPositionReq :
        case FloorBase::SetPositionReq :
        {
            handlePositionReq(msg_in);
            m_channel->update(m_msgout);
            break;
        }
        default:
            fprintf(stderr, "%s received a unknown message: %s\n", 
                    m_name.c_str(), msg_in.toString().c_str());
            break;
        }
    }

    // helpers
    void FloorManager::setInitialSubscriptions() {
        std::set<int> type_set;
        type_set.insert((int)FloorBase::ExecutionReport);
        type_set.insert((int)FloorBase::UserReq);
        type_set.insert((int)FloorBase::ExecutionReplayDone);
        m_channel->addSubscription(type_set);
    }

    void FloorManager::addPositionSubscriptions() {
        std::set<int> type_set;
        type_set.insert((int)FloorBase::SetPositionReq);
        type_set.insert((int)FloorBase::GetPositionReq);
        m_channel->addSubscription(type_set);
    }

    void FloorManager::handleExecutionReport(const MsgType& msg) {
        m_pm.update(*((pm::ExecutionReport*)(msg.buf)));
    }

    void FloorManager::handleUserReq(const MsgType& msg) {
        const char* cmd = msg.buf;
        fprintf(stderr, "%s got user command: %s\n", m_name.c_str(), msg.buf);
        m_msgout.type = FloorBase::UserResp;
        m_msgout.ref = msg.ref;
        const std::string helpstr(
                    "Command Line Interface\n"
                    "P algo=algo_name,symbol=symbol_name\n\tlist positions (and open orders) of the specified algo and sybmol\n\thave to specify both algo and symbol, ALL is reserved for dumping all entries\n"
                    "!B|S algo_name, symbol, qty, price\n\tenter buy or sell with limit order with given qty/px\n"
                    "!A position_line\n\tadjust the position and pnl using the given csv line\n"
                    "!R limit_line\n\tset limit according to the given csv line\n"
                    "!E \n\tinitiate the reconcile process, if good, persist currrent position to EoD file\n"
                    "!D \n\t dump the state of Floor Manager\n"
                    "!!K \n\tstop the message processing and done\n"
                    "H\n\tlist of commands supported\n");

        switch (cmd[0]) {
            case 'H' : 
            {
                m_msgout.copyString(helpstr);
                return;
            };

            case 'P':
            {
                // get position or open order
                std::map<std::string, std::string> key_map;
                if (! parseKeyValue(std::string(cmd+1), key_map)) {
                    m_msgout.copyString(std::string("Failed to parse Algo or Symbol name: ")+ std::string(cmd)+ "\n"+ helpstr);
                } else {
                    m_msgout.copyString(m_pm.toString(&key_map["algo"], &key_map["symbol"]) );
                }
                break;
            }

            case '!' :
            {
                // control messages
                std::string respstr("Ack");
                switch(cmd[1]) {
                    case '!' : 
                    {
                        if (cmd[2] == 'K') {
                            stop();
                        } else {
                            respstr = "Unknow command: " + std::string(cmd, 3)+ "\n" +helpstr;
                        }
                        break;
                    }
                    case 'D' :
                    {
                        respstr = toString();
                        break;
                    }
                    case 'E' :
                    {
                        if (m_eod_pending) {
                            respstr = "Already in EoD\n";
                        } else {
                            PositionManager pmr("reconcile");
                            std::string errstr;
                            if (!requestReplay(pmr.getLoadUtc(), &errstr)) {
                                respstr = "problem requesting replay: " + errstr;
                            } else {
                                m_eod_pending = true;
                            }
                        }
                        break;
                    }
                    case 'B':
                    case 'S':
                    {
                        // send a req and return
                        std::string bsstr(cmd+1);
                        FloorBase::MsgType req(FloorBase::SendOrderReq, bsstr.c_str(), bsstr.size()+1);
                        FloorBase::MsgType resp;
                        if (!m_channel->requestAndCheckAck(req, resp, 1, FloorBase::SendOrderAck)) {
                            respstr = "problem sending order: " + std::string(resp.buf);
                        }
                        break;
                    }

                    default :
                        respstr = "not supported (yet)";
                }
                m_msgout.copyString(respstr);
            };
            default :
                m_msgout.copyString("not supported (yet?)");
        }
    }

    void FloorManager::handlePositionReq(const MsgType msg) {
        m_msgout.ref = msg.ref;
        std::map<std::string, std::string> key_map;
        if (!parseKeyValue(std::string(msg.buf), key_map)) {
            m_msgout.copyString("Parse Error!");
            m_msgout.type = (EventType) ((int)msg.type + 1);
            return;
        }

        if (msg.type == FloorBase::GetPositionReq) {
            // expect a string in format of algo=algo_name,symbol=symbol_name
            // the algo_name is allowed to be "ALL", but symbol has to be specified
            // returns two int64_t as qty and oqty.
            // If algo is "ALL", aggreated qty and oqtya
            m_msgout.type = FloorBase::GetPositionResp;
            int64_t qty[2];
            memset(qty, 0, sizeof(qty));
            qty[0]=m_pm.getPosition(key_map["algo"], key_map["symbol"], nullptr, nullptr, &(qty[1]));
            m_msgout.copyData((char*)qty, sizeof(qty));
            return;
        }

        if(msg.type == FloorBase::SetPositionReq) {
            // expect a string in format of algo=algo_name, symbol=symbol_name, pos=+/-number
            m_msgout.type = FloorBase::SetPositionAck;
            if (key_map.find("algo") == key_map.end() ||
                key_map.find("symbol") == key_map.end() ||
                key_map.find("pos") == key_map.end()) {
                fprintf(stderr, "Setposition parse error: %s\n", msg.buf);
                m_msgout.copyString(std::string("SetPosition parse error: ") + msg.buf);
                return ;
            }
            // TODO - add the trader code here
            m_msgout.copyString("Ack");
        }
    }

    bool FloorManager::requestReplay(const std::string& loadUtc, std::string* errstr) {
        m_recovery_file = loadUtc+"_"+utils::TimeUtil::frac_UTC_to_string(0,3)+"_replay.csv";
        const std::string reqstr = loadUtc + "," + m_pm.getRecoveryPath()+"/"+m_recovery_file;
        MsgType msgReq(FloorBase::ExecutionReplayReq, reqstr.c_str(), reqstr.size()+1);
        MsgType msgResp;
        if(!m_channel->requestAndCheckAck(msgReq, msgResp, 3,  FloorBase::ExecutionReplayAck) ) {
            if (errstr)
                *errstr = std::string(msgResp.buf);
            return false;
        }
        return true;
    }

    bool FloorManager::requestOpenOrder(std::string* errstr) {
        // this should be replayed
        const std::string reqstr = "ALL";
        MsgType msgReq(FloorBase::ExecutionOpenOrderReq, reqstr.c_str(), reqstr.size()+1);
        MsgType msgResp;
        if(!m_channel->requestAndCheckAck(msgReq, msgResp, 3,  FloorBase::ExecutionOpenOrderAck)) {
            if (errstr) {
                *errstr = std::string(msgResp.buf);
            }
            return false;
        }
        return true;
    }

    std::string FloorManager::toString() const {
        char buf[256];
        size_t bytes = snprintf(buf, sizeof(buf),
                "FloorManager %s [running: %s, stopping: %s, loaded: %s, eod_pending: %s, recovery_file: %s, time_loaded: %s]\n"
                "Position Dump\n",
                m_name.c_str(), 
                m_started?"Y":"N", 
                (!m_should_run)?"Y":"N", 
                m_loaded?"Y":"N", 
                m_eod_pending?"Y":"N",
                m_recovery_file.c_str(), 
                utils::TimeUtil::frac_UTC_to_string(m_loaded_time,0).c_str());
        return std::string(buf) + m_pm.toString();
    }

    void FloorManager::stop() {
        fprintf(stderr, "FloorManager %s stop received\n", m_name.c_str());
        m_should_run = false;
    }
};