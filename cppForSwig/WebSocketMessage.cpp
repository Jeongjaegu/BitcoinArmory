////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2018, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "WebSocketMessage.h"
#include "libwebsockets.h"
#include <google/protobuf/io/zero_copy_stream_impl.h>

using namespace ::google::protobuf::io;

////////////////////////////////////////////////////////////////////////////////
//
// WebSocketMessageCodec
//
////////////////////////////////////////////////////////////////////////////////
vector<BinaryData> WebSocketMessageCodec::serialize(uint32_t id,
   const vector<uint8_t>& payload)
{
   BinaryDataRef bdr;
   if(payload.size() > 0)
      bdr.setRef(&payload[0], payload.size());
   return serialize(id, bdr);
}

////////////////////////////////////////////////////////////////////////////////
vector<BinaryData> WebSocketMessageCodec::serialize(uint32_t id,
   const string& payload)
{
   BinaryDataRef bdr((uint8_t*)payload.c_str(), payload.size());
   return serialize(id, bdr);
}

////////////////////////////////////////////////////////////////////////////////
vector<BinaryData> WebSocketMessageCodec::serialize(uint32_t id,
   const BinaryDataRef& payload)
{
   //TODO: less copies, more efficient serialization

   vector<BinaryData> result;
   auto data_len = payload.getSize();
   BinaryData bd_buffer(LWS_PRE);
   BinaryWriter bw;

   //header packet
   bw.put_BinaryData(bd_buffer);
   bw.put_uint16_t(WEBSOCKET_MAGIC_WORD);
   bw.put_uint32_t(id);
   bw.put_var_int(payload.getSize());
   auto room_left = WEBSOCKET_MESSAGE_PACKET_SIZE - bw.getSize() + LWS_PRE;
   auto packet_len = min(room_left, data_len);
   bw.put_BinaryDataRef(payload.getSliceRef(0, packet_len));
   result.push_back(bw.getData());

   if (packet_len < payload.getSize())
   {
      size_t pos = packet_len;
      while(1)
      {
         BinaryWriter bw;

         //leading bytes for lws write routine
         bw.put_BinaryData(bd_buffer);

         auto packet_len = min(
            WEBSOCKET_MESSAGE_PACKET_SIZE, 
            data_len - pos);

         bw.put_BinaryDataRef(payload.getSliceRef(pos, packet_len));
         result.push_back(bw.getData());

         if (packet_len + pos >= payload.getSize())
            break;

         pos += packet_len;
      }
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
bool WebSocketMessageCodec::reconstructFragmentedMessage(
   const vector<BinaryDataRef>& payloadMap, ::google::protobuf::Message* msg)
{
   //this method expects packets in order

   if (payloadMap.size() == 0)
      return false;

   auto count = payloadMap.size();

   //create a zero copy stream from each packet
   vector<ZeroCopyInputStream*> streams;
   streams.reserve(count);
   
   try
   {
      for (auto& dataRef : payloadMap)
      {
         auto stream = new ArrayInputStream(
            dataRef.getPtr(), dataRef.getSize());
         streams.push_back(stream);
      }
   }
   catch (...)
   {
      for (auto& stream : streams)
         delete stream;
      return false;
   }

   //pass it all to concatenating stream
   ConcatenatingInputStream cStream(&streams[0], streams.size());

   //deser message
   auto result = msg->ParseFromZeroCopyStream(&cStream);

   //cleanup
   for (auto& stream : streams)
      delete stream;

   return result;
}

////////////////////////////////////////////////////////////////////////////////
uint32_t WebSocketMessageCodec::getMessageId(const BinaryDataRef& packet)
{
   //sanity check
   if (packet.getSize() < 7)
      return UINT32_MAX;

   return *(uint32_t*)(packet.getPtr() + 2);
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef WebSocketMessageCodec::getSingleMessage(const BinaryDataRef& bdr)
{
   BinaryRefReader brr(bdr);
   brr.advance(6);
   auto len = brr.get_var_int();
   if (len > brr.getSizeRemaining())
      return BinaryDataRef();

   return brr.get_BinaryDataRef(len);
}

///////////////////////////////////////////////////////////////////////////////
//
// WebSocketMessage
//
///////////////////////////////////////////////////////////////////////////////
void WebSocketMessage::construct(uint32_t msgid, vector<uint8_t> data)
{
   packets_ = move(
      WebSocketMessageCodec::serialize(msgid, data));
}

///////////////////////////////////////////////////////////////////////////////
const BinaryData& WebSocketMessage::getNextPacket() const
{
   auto& val = packets_[index_++];
   return val;
}

///////////////////////////////////////////////////////////////////////////////
void WebSocketMessagePartial::reset()
{
   packets_.clear();
   pos_ = len_ = 0;
   id_ = UINT32_MAX;
}

///////////////////////////////////////////////////////////////////////////////
size_t WebSocketMessagePartial::parsePacket(const BinaryDataRef& dataRef)
{
   if (dataRef.getSize() == 0)
      return SIZE_MAX;

   if (packets_.size() == 0)
   {
      auto dataPtr = dataRef.getPtr();

      //look for message header
      auto len = dataRef.getSize();
      size_t i;
      for (i=0; i < len -1; i++)
      {
         auto val = (uint16_t*)(dataPtr + i);
         if (*val == WEBSOCKET_MAGIC_WORD)
            break;
      }

      if (i == len)
         return SIZE_MAX;

      //get slice ref
      auto&& slice = dataRef.getSliceRef(i, dataRef.getSize() - i);
      BinaryRefReader brr(slice);

      brr.advance(2); //skip magic word
      id_ = brr.get_uint32_t(); //message id
      len_ = brr.get_var_int(); //message length

      auto remaining = min(len_, brr.getSizeRemaining());
      auto&& packetRef = brr.get_BinaryDataRef(remaining);
      packets_.push_back(packetRef);
      pos_ = remaining;

      return remaining;
   }
   else
   {
      //fill message
      auto remaining = len_ - pos_;
      if (remaining == 0)
         return SIZE_MAX - 1;

      auto read_size = min(remaining, dataRef.getSize());
      auto&& slice = dataRef.getSliceRef(0, read_size);
      packets_.push_back(slice);
      pos_ += read_size;

      return read_size;
   }
}

///////////////////////////////////////////////////////////////////////////////
bool WebSocketMessagePartial::getMessage(
  ::google::protobuf::Message* msgPtr) const
{
   if (!isReady())
      return false;

   if (packets_.size() == 1)
   {
      auto& dataRef = packets_[0];
      return msgPtr->ParseFromArray(dataRef.getPtr(), dataRef.getSize());
   }
   else
   {
      return WebSocketMessageCodec::reconstructFragmentedMessage(packets_, msgPtr);
   }
}