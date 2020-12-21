
#include "message_handler.hpp" 

const int MessageHandler::TAG_DEFAULT = -42;

MessageHandler::MessageHandler() {

}

void MessageHandler::registerCallback(int tag, const MsgCallback& cb) {
    _callbacks[tag] = cb;
}

void MessageHandler::pollMessages(float elapsedTime) {
    // Process new messages
    auto handle = MyMpi::poll(elapsedTime);
    if (handle) {
        if (_callbacks.count(handle.value().tag)) _callbacks[handle.value().tag](handle.value());
        else if (_callbacks.count(TAG_DEFAULT)) _callbacks[TAG_DEFAULT](handle.value());
    }
}