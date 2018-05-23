#ifndef RTCS_LICODE_CLIENT_H
#define RTCS_LICODE_CLIENT_H

#include <socket.io-client-cpp\sio_client.h>

namespace licode {
    namespace parse {
        std::string getjson(sio::message::ptr msg);
        sio::message::ptr getMessage(const std::string& json);
        bool checkarraybool(const std::string& json);
    }


    class licodestream final {
    public:
        ///** check if this is outgoing (local, true), or incoming (remote, false) */
        //bool isLocal();
        explicit licodestream(int64_t id, const std::string& stream_name);
        ~licodestream();
        /** access this streams unique id */
        int64_t getID() const {
            return _id;
        }

        void setID(int64_t id) {
            _id = id;
        }

        /** access this streams name */
        std::string getName() const {
            return _name;
        }

        void setName(const std::string& name) {
            _name = name;
        }

    private:
        friend class licoderoom;
        licodestream();

        int64_t _id;
        std::string _name;

    };

    class licodeobserver
    {
    public:
        virtual void OnRoomJoind(licoderoom* remoteroom) = 0;
    };

    class licoderoomobserver {
    public:
        virtual void OnNewStream(const licodestream& remotestream) = 0;
        virtual void OnRemoveStream(int64_t streamid) = 0;
        virtual void OnPublish(const licodestream& localstream) = 0;
        virtual void OnUnPublish(int64_t streamid) = 0;
        virtual void OnSubscriber(int64_t streamid, bool audio = true, bool video = true) = 0;
        virtual void OnUnSubscriber(int64_t streamid) = 0;
        virtual void OnRemoteSDP(int64_t streamid, const std::string& remote_sdp) = 0;
    };

    enum class failtype {
        BAD_PARAMETER,
        SOCKET_IO_FAILE,
    };

    template<typename errortype>
    class licodeerror {
    public:
        licodeerror(const std::string& message, const errortype& type) :_message(message), _type(type) {

        }
        ~licodeerror() {

        }

        std::string message() const {
            return _message;
        }

        errortype type() const {
            return _type;
        }

    protected:
        const std::string _message;
        errortype _type;
    };

    using onsuccess = std::function<void()>;

    template<typename T>
    using onfail = std::function<void(const T& type)>;

    class licoderoom {
        friend class licodeclient;
    public:
        void publish(const std::string& stream_name, bool video = true, bool audio = true, const onfail<failtype>& fail = nullptr);
        void unpublish(int64_t streamid, const onfail<failtype>& fail = nullptr);
        void subscriber(int64_t streamid, bool video=true,bool audio=true,const onfail<failtype>& fail = nullptr);
        void unsubscriber(int64_t streamid, bool* hasUnSubscriber, const onfail<failtype>& fail = nullptr);
        void sendSDP(const std::string& sdp, int64_t streamid);
        void sendCandidate(const std::string& candidate, int mline_index,
                           const std::string& sdp_mid, int64_t streamid);

        void bindobserver(licoderoomobserver* _obs) {
            assert(_obs);
            obs = _obs;
        }

        std::string roomdiscription() const{
            return _roomdiscription;
        }

        std::string roomid() const {
            return _roomid;
        }

        std::vector<licodestream> curstreams() const {
            return _roomstreams;
        }

    private:
        licoderoom(const std::string& roomdiscription,const std::string& token);
        ~licoderoom();
        void connect(const onsuccess& success, const onfail<failtype>& fail = nullptr);
        void disconnect();
        void onsocketioconneted();
        void ontokenack(const std::string& json);
        void bindevent();
        void onstreamaddevent(const std::string& json);
        void on_signaling_message_erizo(const std::string& json);
        void onremovestream(const std::string& json);
        void ondisconnectfromserver(const std::string& json);

    private:
        std::string _roomdiscription;
        std::string _roomid;
        std::vector<licodestream> _roomstreams;
#if _DEBUG
        std::vector<licodestream> pubstreams;
#endif
        licoderoomobserver* obs;
        sio::client* mclient;
        std::string mtoken;
        onsuccess connsuccess;

    };

    class licodeclient final
    {
    public:
        licodeclient(licodeobserver* liobs);
        ~licodeclient();

        void joinmcuroom(const std::string& room,const std::string& token, const onfail<failtype>& faile = nullptr);
        void leaveroom(licoderoom* room, const onfail<failtype>& faile = nullptr);
        void RegisterObserver(licodeobserver* callback) { observer = callback; }
    private:
        licodeclient() {};
        licodeclient(const licodeclient&) = delete;
        licodeclient &operator=(const licodeclient &) = delete;

    private:
        licodeobserver * observer;
        onfail<failtype> joinfail;
    };
}


#endif
