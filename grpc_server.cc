/*
 *
 * Copyright 2015 gRPC authors.
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
 *
 */

#include <iostream>
#include <memory>
#include <string>
#include <fstream>
#include <chrono>
#include <iterator>
#include <thread>
#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif
#include "google/cloud/speech/v1/cloud_speech.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using audiostream::HelloRequest;
using audiostream::HelloReply;
using audiostream::Streamer;
using audiostream::SpeechChunk;
using audiostream::TranscriptChunk;
using audiostream::SpeechRecognitionAlternative;

using google::cloud::speech::v1::Speech;
using google::cloud::speech::v1::StreamingRecognizeRequest;
using google::cloud::speech::v1::StreamingRecognizeResponse;
using google::cloud::speech::v1::RecognitionConfig;
using google::cloud::speech::v1::WordInfo;
using google::cloud::speech::v1::SpeechContext;
using google::protobuf::Duration;

// Create a Speech Stub connected to the speech service.
auto creds = grpc::GoogleDefaultCredentials();
auto channel = grpc::CreateChannel("speech.googleapis.com", creds);
std::unique_ptr<Speech::Stub> speech(Speech::NewStub(channel));

auto context = std::make_unique<grpc::ClientContext>();
auto streamer = speech->StreamingRecognize(context.get());

auto global_start = std::chrono::high_resolution_clock::now();
auto last_reset = std::chrono::high_resolution_clock::now();
int flag = 1;
int STREAMING_LIMIT = 29000; // ~5 minutes

void add_google_speech_results(audiostream::SpeechRecognitionAlternative* alt, google::cloud::speech::v1::SpeechRecognitionAlternative google_speech_alternative)
{
  alt->set_transcript(google_speech_alternative.transcript());//"bytes");
  alt->set_confidence(google_speech_alternative.confidence());//0.9);
}

// Dumpt the samples to a raw file 
void WriteToFile(const std::string& strFilename, std::vector<std::string>& audio_chunks)
{
  std::ofstream output_file(strFilename);
  std::ostream_iterator<std::string> output_iterator(output_file);
  std::copy(audio_chunks.begin(), audio_chunks.end(), output_iterator);
}

void add_google_hints(SpeechContext* speech_context, std::vector<std::string>& hints_words)
{
  for(std::string hints_word : hints_words)
  {
    speech_context->add_phrases(hints_word);
  }
  std::cout << " #### Number of hints added: " << speech_context->phrases_size() << "\n";
}

void setGoogleConfigSettings(RecognitionConfig* config)
{
  config->set_language_code("en");
  config->set_sample_rate_hertz(16000);  // Default sample rate.
  config->set_encoding(RecognitionConfig::LINEAR16);
  config->set_enable_word_time_offsets(true);
  config->set_profanity_filter(true);
  config->set_enable_automatic_punctuation(true);
  
  std::vector<std::string> hints_strings;
  hints_strings.push_back("hello");
 
  
  add_google_hints(config->add_speech_contexts(), hints_strings);
  std::cout << " #### Number of speech contexts added: " << config->speech_contexts_size() << "\n";
  // for(std::string hints_string : hints_strings)
  // {
  //   add_google_hints(config->add_speech_contexts(), hints_string);
  // }
  
}

bool IsRecoverable(grpc::Status& grpc_status)
{
  std::cout << " GRPC error detected "<< grpc_status.error_code() << "\tmessage:" << grpc_status.error_message() << "\n";
  if(grpc_status.error_code() == 11) // google throwing exceeded maximum allowed duration of 305 seconds
  {
    return true;
  }

  return false;
}

auto reset_stream = [&context, &streamer, &last_reset]
{
    std::cout << "&&&& NOTE::: RESETING STREAM at " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now()-global_start).count() <<"\n";
    // Reset the stream
    context->TryCancel();
    auto status = streamer->Finish();
    if (not IsRecoverable(status)) {
      //TODO throw GrpcStatusToException(status);
      std::cout << "throw some exception here \n";
      //return;
    }
    context = std::make_unique<grpc::ClientContext>();
    streamer = speech->StreamingRecognize(context.get());
    last_reset = std::chrono::high_resolution_clock::now();
};

// Write the audio in 100 ms chunks at a time, simulating audio content arriving from a microphone.
static int MicrophoneThreadMain(
    grpc::ClientReaderWriterInterface<StreamingRecognizeRequest,
                                      StreamingRecognizeResponse>* streamer, ServerReaderWriter<TranscriptChunk, SpeechChunk>* stream,
                                      std::vector<std::string>* audio_vector)
{
  flag = 1;
  SpeechChunk speech_chunk;
  StreamingRecognizeRequest request;
  while (stream->Read(&speech_chunk)) {
    std::string audio_content = speech_chunk.audio_content();
    //std::cout << "aduio content " << audio_content << "\n";
    request.set_audio_content(audio_content);
    //std::cout << " received " << audio_content.length() << " \n"; 
    //std::cout << "Sending " << audio_content.length() / 1024 << "k bytes." << std::endl;
    streamer->Write(request);
    if(flag == 1) 
    {
      global_start = std::chrono::high_resolution_clock::now();
      std::cout << "####### global_start timer updated ... " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()<< "\n";
      flag = 2;
    }
    audio_vector->push_back(speech_chunk.audio_content());
  }
  std::cout << "closing stream.....\n";
  streamer->WritesDone();
  std::cout << "closed stream after writes done .....\n";
  return 0;
}
// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Streamer::Service 
{
  Status SayHello(ServerContext* context, const HelloRequest* request,
                  HelloReply* reply) override 
  {
    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());
    std::cout << "hello world received >>>> \n";
    return Status::OK;
  }

  Status StreamAudio(ServerContext* context, ServerReaderWriter<TranscriptChunk, SpeechChunk>* stream) override {
      std::cout << "####### Started receiving stream ... " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()<< "\n";

      StreamingRecognizeRequest request;
      auto* streaming_config = request.mutable_streaming_config();
      setGoogleConfigSettings(streaming_config->mutable_config());
      streaming_config->set_interim_results(true);
      
      // Write the first request, containing the config only.
      streamer->Write(request);

      std::vector<std::string> audio_bytes;
      
      // The microphone thread writes the audio content.
      std::thread microphone_thread(&MicrophoneThreadMain, streamer.get(), stream, &audio_bytes);
      std::cout << "Reading responses....\n";
      
      // Read responses.
      StreamingRecognizeResponse response;
      auto full_start = std::chrono::high_resolution_clock::now();
      auto start = std::chrono::high_resolution_clock::now();
      while (streamer->Read(&response)) // Returns false when no more to read.
      {
        
        //std::cout << "### resp =======> " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now()-global_start).count() << " ms \n";
        auto resp_diff_with_global = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now()-global_start).count();
        // Dump the transcript of all the results.
        for (int r = 0; r < response.results_size(); ++r) 
        {
          const auto& result = response.results(r);
          TranscriptChunk transcript;
          transcript.set_is_final(result.is_final());
          transcript.set_stability(result.stability());
          //std::cout << "Result stability: " << result.stability() << std::endl;
          float stability_number = result.stability()*1.0;
          for (int a = 0; a < result.alternatives_size(); ++a) 
          {
            const auto& alternative = result.alternatives(a);
            add_google_speech_results(transcript.add_alternatives(), alternative);
            auto words = alternative.words();
            // std::cout << " ######" <<  stability_number << "\t" <<alternative.confidence() << "\t"
            //           << alternative.transcript() << " ..... " << words.size() << std::endl;
            if(stability_number >= 0.1)
            {
              std::cout << " ######" <<  stability_number <<  " is_final " << result.is_final() <<"\t" << alternative.transcript() << "\t" << resp_diff_with_global << " ms\n"; 
            }
            for(int i = 0; i < words.size(); i++) 
            {
              // Duration t1 = words[i].start_time();
              // Duration t2 = words[i].end_time();
              const auto& word = alternative.words(i);
              float start_secs = word.start_time().seconds() * 1000.0 + word.start_time().nanos()*1.0/1000000;
              float end_secs = word.end_time().seconds() * 1000.0 + word.end_time().nanos()*1.0/1000000;

              std::cout << word.word()  << " "<< start_secs  << " ms -  " << end_secs << " ms" << "\n";

            }
          }
          
          if(result.is_final())
          {
            std::cout << "final -----> " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now()-global_start).count() << " ms \n";
            start = std::chrono::high_resolution_clock::now();
          }
          
          stream->Write(transcript);

          if(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now()-last_reset).count() >= STREAMING_LIMIT)
          {
            std::cout << " resetting stream because of timer \n";
            reset_stream();
          }
        }
      }
      grpc::Status status = streamer->Finish();
      if(IsRecoverable(status))
      {
        std::cout << " resetting stream because of streamer finish caused some issues here \n";
        reset_stream();
      }

      microphone_thread.join();
      if (!status.ok()) {
        // Report the RPC failure.
        std::cerr << status.error_message() << std::endl;
        return Status::OK;
      }
      std::cout << " Writing to a file ... \n";
      WriteToFile("output.raw", audio_bytes);
      return Status::OK;
  }
};

void RunServer() {
  std::string server_address("0.0.0.0:50051");
  GreeterServiceImpl service;

  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  RunServer();

  return 0;
}
