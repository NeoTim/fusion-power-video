// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef FUSION_POWER_VIDEO_H_
#define FUSION_POWER_VIDEO_H_

#include <stdint.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fpvc {

// Helper function to convert 16-bit image frames to/from the raw file format.

// Converts 16-bit frame back to the original raw file format.
void UnextractFrame(const uint16_t* img,
                    size_t xsize, size_t ysize, int shift,
                    bool big_endian, uint8_t* out);

// Streaming decoder
class StreamingDecoder {
 public:
  /* Decodes frames in a streaming fashing. Appends the given bytes to the
  input buffer. Calls the callback function for all decoded frames that could
  be decoded so far. The payload is an optional parameter to pass on to the
  callback. */
  void Decode(const uint8_t* bytes, size_t size,
      std::function<void(bool ok, uint16_t* frame, size_t xsize, size_t ysize,
          void* payload)> callback,
      void* payload = nullptr);

 private:
  size_t xsize;
  size_t ysize;

  size_t id = 0;

  std::vector<uint16_t> delta_frame;

  std::vector<uint8_t> buffer;
};

enum FrameState {
  EMPTY = 0,
  RAW = 1,
  PREVIEW_GENERATED = 2,
  DELTA_PREDICTED = 4,
  CG_PREDICTED = 8,
  COMPRESSED = 16,
};

enum FrameFlags {
  NONE = 0,
  USE_DELTA = 1,
  USE_CG = 2,
  NO_LOW_BYTES = 4,
};

class Frame {
  size_t xsize_ = 0;
  size_t ysize_ = 0;
  size_t size_ = 0;
  uint8_t flags_ = FrameFlags::NONE; // FrameFlags
  uint8_t state_ = FrameState::EMPTY; // FrameState
  int64_t timestamp_;

 protected:
  std::vector<uint8_t> preview_;
  std::vector<uint8_t> high_;
  std::vector<uint8_t> low_;

 public:
  static Frame EMPTY;

  size_t xsize() const { return xsize_; }
  size_t ysize() const { return ysize_; }
  uint8_t flags() const { return flags_; }
  uint8_t state() const { return state_; }
  int64_t timestamp() const { return timestamp_; }
  const std::vector<uint8_t> &high() { return high_; }
  const std::vector<uint8_t> &low() { return low_; }
  const std::vector<uint8_t> &preview() { return preview_; }
  std::vector<uint8_t> &&MoveOutHigh() { return std::move(high_); }
  std::vector<uint8_t> &&MoveOutLow() { return std::move(low_); }
  std::vector<uint8_t> &&MoveOutPreview() { return std::move(preview_); }

  Frame(size_t xsize = 0, size_t ysize = 0, const uint16_t* image = nullptr,
        int shift_to_left_align = 0, bool big_endian = false, int64_t timestamp = -1);
  Frame(size_t xsize, size_t ysize, const uint8_t* image, int64_t timestamp = -1);
  Frame(size_t xsize, size_t ysize, uint8_t flags, uint8_t state, std::vector<uint8_t> &&high, 
        std::vector<uint8_t> &&low, std::vector<uint8_t> &&preview, int64_t timestamp = -1);

  static size_t MaxCompressedPlaneSize(size_t xsize, size_t ysize);
  static size_t MaxCompressedPreviewSize(size_t xsize, size_t ysize);

  size_t MaxCompressedPlaneSize();
  size_t MaxCompressedPreviewSize();

  void Compress(Frame &delta_frame = EMPTY);
  void Uncompress(Frame &delta_frame = EMPTY);
  void Predict(Frame &delta_frame = EMPTY);
  void CompressPredicted(size_t* encoded_high_size, uint8_t* encoded_high_buffer,
    size_t* encoded_low_size, uint8_t* encoded_low_buffer,
    size_t* encoded_preview_size, uint8_t* encoded_preview_buffer, bool parallel = true);
  void OutputCore(std::vector<uint8_t> *out);
  void OutputFull(std::vector<uint8_t> *out);
  
 private:

  void GeneratePreview();
  void OptionallyApplyDeltaPrediction(Frame &delta_frame);
  void OptionallyApplyClampedGradientPrediction();
  void ApplyBrotliCompression();
  void ApplyBrotliCompression(size_t* encoded_high_size, uint8_t* encoded_high_buffer,
    size_t* encoded_low_size, uint8_t* encoded_low_buffer,
    size_t* encoded_preview_size, uint8_t* encoded_preview_buffer,
    bool parallel = true);

  void UnapplyBrotliCompression();
  void OptionallyUnapplyDeltaPrediction(Frame &delta_frame);
  void OptionallyUnapplyClampedGradientPrediction();
  
};

// Rnadom access decoder: requires random access to the entire data file,
// can decode any frame in any order.
class RandomAccessDecoder {
 public:
   // Parses the header and footer, must be called once with the full data
   // before using DecodeFrame, xsize, ysize or numframes.
   bool Init(const uint8_t* data, size_t size);

   // Decodes the frame with the given index. The index must be smaller than
   // numframes. The output frame must have xsize * ysize values.
   bool DecodeFrame(size_t index, uint16_t* frame) const;

   bool DecodePreview(size_t index, uint8_t* preview) const;

   size_t xsize() const { return xsize_; }
   size_t ysize() const { return ysize_; }

   // Returns the dimensions of preview images
   size_t preview_xsize() const { return xsize_ / 4; }
   size_t preview_ysize() const { return ysize_ / 4; }

   // Returns amount of frames in the full file.
   size_t numframes() const { return frame_offsets.size(); }

 private:
  size_t xsize_ = 0;
  size_t ysize_ = 0;
  std::vector<uint16_t> delta_frame;
  std::vector<size_t> frame_offsets;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// Multithreaded encoder.
class Encoder {
 public:
  // Uses num_threads worker threads, or disables multithreading if num_threads
  // is 0.
  Encoder(size_t num_threads = 8, int shift_to_left_align = 0, bool big_endian = false);

  // The payload is an optional argument to pass from calls to the callback.
  typedef std::function<void(const uint8_t* compressed, size_t size,
      void* payload)> Callback;

  /* Initializes before the first frame, and writes the header bytes by
  outputting them to the callback.
  The delta_frame must have xsize * ysize pixels. */
  void Init(const uint16_t* delta_frame, size_t xsize, size_t ysize,
      Callback callback, void* payload);

  /* Queues a single 16-bit grayscale frame for encoding.
  The frame should be in the format extracted from the raw data using using
  ExtractFrame.
  Calls the callback function when finished compressing, asynchronously but
  guarded and guaranteed in the correct order. The payload can optionally be
  used to bind an extra argument to pass to the callback.
  User must manage memory of img: it must exist until the callback for this
  frame is called. There can exist up to MaxQueued() tasks at the same time so
  at least that many seperate img memory buffers have to exist at the same time.
  Init must be called before compressing the first frame, and Finish must be
  called after the last frame was queued.*/
  void CompressFrame(const uint16_t* img, Callback callback, void* payload);

  /* Waits and finishes all threads, and writes the footer bytes by
  outputting them to the callback. */
  void Finish(Callback callback, void* payload);

  /* Returns the max amount of frames that can be queued and/or being processed
  at the same time for multithreaded processing. This could be larger than the
  amount of worker threads. */
  size_t MaxQueued() const;

 private:
  struct Task {
    const uint16_t* frame;
    size_t id;
    Callback callback;
    void* payload;
  };

  void RunThread();

  std::vector<uint8_t> RunTask(const Task& task);

  // Finalize a task, unlike RunTask this is guaranteed to run in sequential
  // order and guarded.
  void FinishTask(const Task& task, std::vector<uint8_t>* compressed);

  void WriteFrameIndex(std::vector<uint8_t>* compressed) const;

  std::vector<std::thread*> threads;
  std::mutex m;

  std::condition_variable cv_in;  // for incoming frames
  std::queue<Task> q_in;

  std::condition_variable cv_out;  // for outputting compressed frames
  std::queue<Task> q_out;

  std::condition_variable cv_main;  // for the main thread

  bool finish = false;

  size_t id = 0;  // Unique frame id.

  size_t xsize_;
  size_t ysize_;

  Frame delta_frame_;
  std::vector<size_t> frame_offsets;
  size_t bytes_written = 0;

  int shift_to_left_align_ = 0;
  bool big_endian_ = false;
};

}  // namespace fpvc

#endif  // FUSION_POWER_VIDEO_H_
