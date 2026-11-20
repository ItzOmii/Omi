/* The smallest embedder that proves the engine actually runs: compile a script,
 * run it, get the value back. If this prints, V8's isolate, heap, JIT and
 * platform threads are all working on Omi. */
#include <cstdio>
#include <memory>

#include "libplatform/libplatform.h"
#include "v8-context.h"
#include "v8-initialization.h"
#include "v8-isolate.h"
#include "v8-local-handle.h"
#include "v8-primitive.h"
#include "v8-script.h"
#include "v8-statistics.h"

int main(int argc, char **argv) {
  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  int failed = 0;
  {
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    v8::Isolate *isolate = v8::Isolate::New(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);

      const char *src =
          "let s = 0; for (let i = 0; i < 100000; i++) s += i;"
          " 'js says ' + s + ' and 2+2 is ' + (2+2)";
      v8::Local<v8::String> source =
          v8::String::NewFromUtf8(isolate, src).ToLocalChecked();
      v8::Local<v8::Script> script;
      if (!v8::Script::Compile(context, source).ToLocal(&script)) {
        std::printf("[v8] FAIL compile\n");
        failed = 1;
      } else {
        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result)) {
          std::printf("[v8] FAIL run\n");
          failed = 1;
        } else {
          v8::String::Utf8Value utf8(isolate, result);
          std::printf("[v8] %s\n", *utf8);
          v8::HeapStatistics stats;
          isolate->GetHeapStatistics(&stats);
          std::printf("[v8] heap used %d KB, version %s\n",
                      (int)(stats.used_heap_size() / 1024), v8::V8::GetVersion());
        }
      }
    }
    isolate->Dispose();
  }
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  /* Leak the platform on purpose. `platform` is a stack object, so returning
   * from main runs ~DefaultPlatform, and that destructor waits on the worker
   * threads it just asked to stop: on Omi that wait never returns, which reads
   * as "exit() hangs" - main never gets as far as exit(), no atexit handler
   * runs, and the process is never reaped. V8's own samples leak the platform
   * for exactly this reason. The open question is which thread posts the
   * semaphore it is waiting on - see V8.md. */
  platform.release();
  std::printf("[v8] %s\n", failed ? "not yet" : "engine ran");
  std::fflush(nullptr);
  return failed;
}