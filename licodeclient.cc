#include "licodeclient.h"
#include <regex>
#include <assert.h>
#include <cppcodec/base64_default_rfc4648.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>
#include <mutex>
#include "common/trace_out.h"
#include "common/string_util.h"
#include "socket.io-client-cpp/internal/sio_packet.h"

namespace licode {
    namespace parse {
        static sio::packet_manager manager;
        static std::mutex packetLock;
        std::string getjson(sio::message::ptr msg) {
            std::lock_guard< std::mutex > guard(packetLock);
            std::stringstream ss;
            sio::packet packet("/", msg);
            manager.encode(packet, [&](bool isBinary, std::shared_ptr<const std::string> const& json)
            {
                ss << *json;
                assert(!isBinary);
            });
            manager.reset();

            // Need to strip off the message type flags (typically '42',
            // but there are other possible combinations).
            std::string result = ss.str();
            std::size_t indexList = result.find('[');
            std::size_t indexObject = result.find('{');
            std::size_t indexString = result.find('"');
            std::size_t index = indexList;
            if (indexObject != std::string::npos && indexObject < index)
                index = indexObject;
            if (indexString != std::string::npos && indexString < index)
                index = indexString;

            if (index == std::string::npos) {
                assert(true);
                return "";
            }
            return result.substr(index);
        }
        sio::message::ptr getMessage(const std::string& json) {
            std::lock_guard< std::mutex > guard(packetLock);
            sio::message::ptr message;
            manager.set_decode_callback([&](sio::packet const& p)
            {
                message = p.get_message();
            });

            // Magic message type / ID
            std::string payload = std::string("42") + json;
            manager.put_payload(payload);

            manager.reset();
            return message;
        }

        bool checkarraynull(const std::string& json) {
            rapidjson::Document document;
            if (document.Parse<0>(json.c_str()).HasParseError())
            {
                return false;
            }
            if (!document.IsArray()) {
                return false;
            }
            if (document[0].IsNull()) {
                return false;
            }
            return true;
        }

        bool checkarraybool(const std::string& json) {
            rapidjson::Document document;
            if (document.Parse<0>(json.c_str()).HasParseError())
            {
                return false;
            };
            if (!document.IsArray()) {
                return false;
            }
            if (document[0].IsNull()) {
                return false;
            }
            if (document[0].IsBool()) {
                return true;
            }
            /*if (document[0].GetBool()) {
                return true;
            }*/

            return false;
        }
    }

    licodestream::licodestream() {

    }

    licodestream::licodestream(int64_t id, const std::string& stream_name)
        :_id(id),
         _name(stream_name){

    }

    licodestream::~licodestream() {

    }

    licoderoom::licoderoom(const std::string& roomdiscription,const std::string& token)
        :_roomdiscription(roomdiscription),mtoken(token), mclient(0), obs(0) {

    }

    void licoderoom::connect(const onsuccess& success, const onfail<failtype>& fail) {
        if (success) {
            connsuccess = success;
        }
        rapidjson::Document document;
        if (document.Parse<0>(mtoken.c_str()).HasParseError())
        {
            fail(failtype::BAD_PARAMETER);
            return;
        };

        if (!document.HasMember("host")) {
            fail(failtype::BAD_PARAMETER);
            return;
        }

        std::string host = document["host"].GetString();

        if (!ddscore::util::hasprefixstring(host, "http"))
        {
            host = "http://" + host;
        }

        // socketio client
        mclient = new sio::client();
        mclient->set_open_listener([&]() {
            $trace_p("socketio connected");

            bindevent();

            onsocketioconneted();
        });
        mclient->set_close_listener([](sio::client::close_reason const& reason) {
            $trace_p("socketio closed");
        });
        mclient->set_fail_listener([&]() {
            fail(failtype::SOCKET_IO_FAILE);
        });
        mclient->connect(host);
    }

    void licoderoom::disconnect() {
        if (mclient) {
            mclient->sync_close();
        }
    }

    licoderoom::~licoderoom() {
        if (mclient)
        {
            mclient->clear_con_listeners();
            mclient->clear_socket_listeners();
            delete mclient;
            mclient = nullptr;
        }
    }



    void licoderoom::onsocketioconneted() {
        sio::socket::ptr socket = mclient->socket();
        sio::message::ptr token_ptr = parse::getMessage(mtoken);
        // licode use json object
        socket->emit("token", token_ptr, [this](const sio::message::list& acks) {
            std::string json = parse::getjson(acks.to_array_message());
            $trace_p(json);
            ontokenack(json);
        });
    }

    void licoderoom::ontokenack(const std::string& json) {
        rapidjson::Document ack_doc;
        if (ack_doc.Parse<0>(json.c_str()).HasParseError())
            return;
        assert(ack_doc.IsArray());
        std::string first = ack_doc.GetArray().Begin()->GetString();
        if ("success" == first)
        {
            // send success
            rapidjson::Value& metaObject = ack_doc[1];
            _roomid = metaObject["id"].GetString();


            rapidjson::Value streams = metaObject["streams"].GetArray();
            for (rapidjson::SizeType i = 0; i < streams.Size(); i++) {
                rapidjson::Value attribute = streams[i]["attributes"].GetObject();

                std::string stream_name = attribute["name"].GetString();

                int64_t stremid = streams[i]["id"].GetInt64();

                _roomstreams.push_back(licodestream(stremid, stream_name));
            }
            connsuccess();
        }
    }

    void licoderoom::onstreamaddevent(const std::string& json) {
        rapidjson::Document document;
        if (document.Parse<0>(json.c_str()).HasParseError())
        {
            return;
        };

        if (!document.HasMember("id"))
            return;
        if (!document.HasMember("attributes"))
            return;

        rapidjson::Value attribute = document["attributes"].GetObject();
        if (!attribute.IsObject())
            return;
        std::string stream_name = attribute["name"].GetString();

        if (obs) {
            licodestream newstream(document["id"].GetInt64(), stream_name);
            _roomstreams.push_back(newstream);
            obs->OnNewStream(newstream);
        }
    }

    void licoderoom::unpublish(int64_t streamid, const onfail<failtype>& fail) {
        // send unpublish message
        rapidjson::StringBuffer s;
        rapidjson::Writer<rapidjson::StringBuffer> writer(s);
        writer.StartArray();
        writer.Int64(streamid);
        writer.EndArray();

        sio::message::ptr jsonobj = parse::getMessage(s.GetString());
        sio::socket::ptr socket = mclient->socket();

        socket->emit("unpublish", jsonobj, [=](const sio::message::list& acks) {
            // ack callback get publish streamid--int64
            std::string json = parse::getjson(acks.to_array_message());

            if (parse::checkarraybool(json) && obs) {
                obs->OnUnPublish(streamid);
            }
        });
    }

    void licoderoom::unsubscriber(int64_t streamid, bool* hasUnSubscriber, const onfail<failtype>& fail) {
        // send unsubscriber message
        rapidjson::StringBuffer s;
        rapidjson::Writer<rapidjson::StringBuffer> writer(s);
        writer.StartArray();
        writer.Int64(streamid);
        writer.EndArray();

        sio::message::ptr jsonobj = parse::getMessage(s.GetString());
        sio::socket::ptr socket = mclient->socket();

        socket->emit("unsubscribe", jsonobj, [=](const sio::message::list& acks) {
            // ack callback get publish streamid--int64
            std::string json = parse::getjson(acks.to_array_message());
            if (parse::checkarraybool(json) && obs) {
                obs->OnUnSubscriber(streamid);
            }
            if (hasUnSubscriber != nullptr)
                * hasUnSubscriber = true;
        });

    }

    void licoderoom::publish(const std::string& stream_name, bool video, bool audio, const onfail<failtype>& fail) {
        // send publish message get streamid
        rapidjson::StringBuffer s;
        rapidjson::Writer<rapidjson::StringBuffer> writer(s);
        writer.StartObject();
        writer.Key("state");
        writer.String("erizo");
        writer.Key("data");
        writer.Bool(false);
        writer.Key("audio");
        writer.Bool(audio);
        writer.Key("video");
        writer.Bool(video);
        writer.Key("minVideoBW");
        writer.Int(0);
        writer.Key("attributes");
        writer.StartObject();
        writer.Key("name");
        writer.String(stream_name.c_str());
        writer.Key("type");
        writer.String("public");
        writer.EndObject();
        writer.EndObject();

        sio::message::ptr jsonobj = parse::getMessage(s.GetString());
        sio::socket::ptr socket = mclient->socket();

        socket->emit("publish", jsonobj, [=](const sio::message::list& acks) {
            // ack callback get publish streamid--int64
            std::string json = parse::getjson(acks.to_array_message());
            $trace_p(json);

            if (obs && parse::checkarraynull(json)) {
                int64_t id = ddscore::util::strtoi<int64_t>(json.substr((json.find_first_of("[") + 1), json.find_first_of("]")));
#if _DEBUG
                licodestream pubstream(id, stream_name);
                pubstreams.push_back(pubstream);
#endif

                obs->OnPublish(licodestream(id, stream_name));
            }
        });
    }

    void licoderoom::subscriber(int64_t streamid, bool video, bool audio, const onfail<failtype>& fail) {
        // send publish message get streamid
        rapidjson::StringBuffer s;
        rapidjson::Writer<rapidjson::StringBuffer> writer(s);
        writer.StartObject();
        writer.Key("streamId");
        writer.Int64(streamid);
        writer.Key("slideShowMode");
        writer.Bool(false);
        writer.Key("audio");
        writer.Bool(audio);
        writer.Key("video");
        writer.Bool(video);
        writer.EndObject();

        sio::message::ptr jsonobj = parse::getMessage(s.GetString());
        sio::socket::ptr socket = mclient->socket();

        socket->emit("subscribe", jsonobj, [=](const sio::message::list& acks) {
            std::string json = parse::getjson(acks.to_array_message());
            $trace_p(json);
            if (parse::checkarraybool(json)) {
                if (obs) {
                    obs->OnSubscriber(streamid, audio, video);
                }
            }
        });
    }

    //void licoderoom::sendsdpwithcandidate(const std::string& sdp, int64_t streamid) {
    void licoderoom::sendSDP(const std::string& sdp, int64_t streamid) {
        // on sucess send offer sdp
        rapidjson::StringBuffer s;
        rapidjson::Writer<rapidjson::StringBuffer> writer(s);
        writer.StartObject();
        writer.Key("streamId");
        writer.Int64(streamid);
        writer.Key("msg");
        writer.StartObject();
        writer.Key("type");
        writer.String("offer");
        writer.Key("sdp");
        writer.String(sdp.c_str());
        writer.EndObject();
        writer.EndObject();

        sio::message::ptr jsonobj = parse::getMessage(s.GetString());
        sio::socket::ptr socket = mclient->socket();

        socket->emit("signaling_message", jsonobj, [this](const sio::message::list& acks) {
            std::string json = parse::getjson(acks.to_array_message());
            $trace_p(json);
        });
    }

    void licoderoom::sendCandidate(const std::string& candidate, int mline_index,
                                   const std::string& sdp_mid, int64_t streamid) {
        rapidjson::StringBuffer s;
        rapidjson::Writer<rapidjson::StringBuffer> writer(s);
        writer.StartObject();
        writer.Key("streamId");
        writer.Int64(streamid);
        writer.Key("msg");
        writer.StartObject();
        writer.Key("type");
        writer.String("candidate");
        writer.Key("candidate");
        writer.StartObject();
        writer.Key("sdpMLineIndex");
        writer.Int(mline_index);
        writer.Key("sdpMid");
        writer.String(sdp_mid.c_str());
        writer.Key("candidate");
        writer.String(candidate.c_str());
        writer.EndObject();
        writer.EndObject();
        writer.EndObject();

        sio::message::ptr jsonobj = parse::getMessage(s.GetString());
        sio::socket::ptr socket = mclient->socket();

        socket->emit("signaling_message", jsonobj, [this](const sio::message::list& acks) {
            std::string json = parse::getjson(acks.to_array_message());
            $trace_p(json);
        });
    }

    void licoderoom::onremovestream(const std::string& json) {
        rapidjson::Document doc;
        if (doc.Parse<0>(json.c_str()).HasParseError())
        {
            $trace_p("pase json fail");
            return;
        }

        if (doc.HasMember("id")) {
            int64_t streamid = doc["id"].GetInt64();
            if (obs) {

                std::vector<licodestream>::const_iterator iter;
                for (iter = _roomstreams.begin(); iter != _roomstreams.end(); iter++) {
                    if (iter->getID() == streamid)
                        break;
                }
                if (iter != _roomstreams.end())
                    _roomstreams.erase(iter);

#if _DEBUG
                /*for (const auto& pubstream : pubstreams) {
                    assert(pubstream.getID() == streamid);
                }*/
#endif

                obs->OnRemoveStream(streamid);
            }
        }
    }

    void licoderoom::on_signaling_message_erizo(const std::string& json) {
        rapidjson::Document sigmsg;
        if (sigmsg.Parse<0>(json.c_str()).HasParseError())
            return;

        if (sigmsg.HasMember("mess"))
        {
            // has mess
            rapidjson::Value& metaObject = sigmsg["mess"];
            if (metaObject.HasMember("type"))
            {
                std::string answer = metaObject["type"].GetString();
                if (answer == "answer")
                {
                    std::string remotesdp = metaObject["sdp"].GetString();

                    int64_t streamid;

                    if (sigmsg.HasMember("streamId"))
                        streamid = sigmsg["streamId"].GetInt64();
                    else if (sigmsg.HasMember("peerId"))
                        streamid = sigmsg["peerId"].GetInt64();

                    if (obs) {
                        obs->OnRemoteSDP(streamid, remotesdp);
                    }
                }
            }
        }
    }


    void licoderoom::bindevent() {
        // register event
        sio::socket::ptr socket = mclient->socket();

        socket->on("onAddStream",
            sio::socket::event_listener_aux([&](
                std::string const& name,
                sio::message::ptr const& data,
                bool isAck,
                sio::message::list &ack_resp) {
            $trace_p("on AddStream event");
            std::string jsondata = parse::getjson(data);
            $trace_p(jsondata);
            onstreamaddevent(jsondata);
        }));
        socket->on("signaling_message_erizo",
            sio::socket::event_listener_aux([&](
                std::string const& name,
                sio::message::ptr const& data,
                bool isAck,
                sio::message::list &ack_resp) {
            $trace_p("on signaling_message_erizo");
            std::string jsondata = parse::getjson(data);
            $trace_p(jsondata);
            on_signaling_message_erizo(jsondata);
        }));
        socket->on("signaling_message_peer",
            sio::socket::event_listener_aux([&](
                std::string const& name,
                sio::message::ptr const& data,
                bool isAck,
                sio::message::list &ack_resp) {
            std::string jsondata = parse::getjson(data);
            $trace_p("on signaling_message_peer not implementation yet");

        }));
        socket->on("publish_me", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on publish_me  not implementation yet");

        }));
        socket->on("onBandwidthAlert", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on onBandwidthAlert  not implementation yet");

        }));
        socket->on("unpublish_me", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on unpublish_me  not implementation yet");

        }));
        socket->on("onSubscribeP2P", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on SubscribeP2P  not implementation yet");

        }));
        socket->on("onPublishP2P", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on PublishP2P  not implementation yet");
        }));
        socket->on("onPublishP2P", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on PublishP2P  not implementation yet");
        }));
        socket->on("onDataStream", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on DataStream  not implementation yet");
        }));
        socket->on("onRemoveStream", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            // if someone unpublish then will removestream
            std::string jsondata = parse::getjson(data);
            $trace_p(jsondata);
            onremovestream(jsondata);

        }));
        socket->on("onUpdateAttributeStream", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on onUpdateAttributeStream not implementation yet");

        }));
        socket->on("disconnect", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on disconnect not implementation yet");

        }));
        socket->on("connection_failed", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on connection_failed not implementation yet");

        }));
        socket->on("error", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on error not implementation yet");

        }));
        socket->on("connect_error", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on connect_error not implementation yet");

        }));
        socket->on("connect_timeout", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on connect_timeout not implementation yet");

        }));
        socket->on("reconnecting", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on reconnecting not implementation yet");

        }));
        socket->on("reconnect", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on reconnect not implementation yet");

        }));
        socket->on("reconnect_attempt", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on reconnect_attempt not implementation yet");

        }));
        socket->on("reconnect_error", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on reconnect_error not implementation yet");

        }));
        socket->on("reconnect_failed", sio::socket::event_listener_aux([&](
            std::string const& name,
            sio::message::ptr const& data,
            bool isAck,
            sio::message::list &ack_resp) {
            $trace_p("on reconnect_failed not implementation yet");

        }));
    }


    licodeclient::licodeclient(licodeobserver* liobs) :observer(liobs)
    {

    }

    licodeclient::~licodeclient()
    {

    }

    void licodeclient::joinmcuroom(const std::string& room, const std::string& token, const onfail<failtype>& fail)
    {
        if (fail)
        {
            joinfail = fail;
        }
        
        const static char* regex = "^([A-Za-z0-9+/]{4})*([A-Za-z0-9+/]{4}|[A-Za-z0-9+/]{3}=|[A-Za-z0-9+/]{2}==)$";

        std::regex base64regex(regex);

        if (!std::regex_match(token, base64regex, std::regex_constants::match_flag_type::match_default))
        {
            fail(failtype::BAD_PARAMETER);
            return;
        }

        std::string decodetoken = base64::decode<std::string>(token);

        licoderoom* joinroom = new licoderoom(room,decodetoken);

        auto success = [=]() {
            observer->OnRoomJoind(joinroom);
        };

        auto connfail = [=](const failtype& type) {
            joinfail(type);
            if (joinroom) {
                delete joinroom;
            }
        };

        joinroom->connect(success, connfail);
    }

    void licodeclient::leaveroom(licoderoom* room, const onfail<failtype>& faile) {
        assert(room);
        room->disconnect();
        delete room;
        room = nullptr;
    }
}