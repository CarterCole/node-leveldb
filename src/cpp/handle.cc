#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include <algorithm>
#include <sstream>
#include <vector>

#include <leveldb/db.h>
#include <node.h>
#include <node_buffer.h>
#include <v8.h>

#include "batch.h"
#include "handle.h"
#include "helpers.h"
#include "iterator.h"
#include "options.h"

using namespace node;
using namespace v8;

namespace node_leveldb {

Persistent<FunctionTemplate> JHandle::constructor;

static inline Handle<Value> Error(const leveldb::Status& status) {
  if (status.IsNotFound()) {
    return Undefined();
  } else {
    return ThrowError(status.ToString().c_str());
  }
}

void JHandle::Initialize(Handle<Object> target) {
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(New);
  constructor = Persistent<FunctionTemplate>::New(t);
  constructor->InstanceTemplate()->SetInternalFieldCount(1);
  constructor->SetClassName(String::NewSymbol("Handle"));

  // Instance methods
  NODE_SET_PROTOTYPE_METHOD(constructor, "get", Read);
  NODE_SET_PROTOTYPE_METHOD(constructor, "write", Write);
  NODE_SET_PROTOTYPE_METHOD(constructor, "iterator", Iterator);
  NODE_SET_PROTOTYPE_METHOD(constructor, "snapshot", Snapshot);
  NODE_SET_PROTOTYPE_METHOD(constructor, "property", Property);
  NODE_SET_PROTOTYPE_METHOD(constructor, "approximateSizes", ApproximateSizes);
  NODE_SET_PROTOTYPE_METHOD(constructor, "compactRange", CompactRange);

  // Static methods
  NODE_SET_METHOD(target, "open", Open);
  NODE_SET_METHOD(target, "destroy", Destroy);
  NODE_SET_METHOD(target, "repair", Repair);

  // Set version
  std::stringstream version;
  version << leveldb::kMajorVersion << "." << leveldb::kMinorVersion;
  target->Set(String::New("bindingVersion"),
              String::New(version.str().c_str()),
              static_cast<PropertyAttribute>(ReadOnly|DontDelete));
}

Handle<Value> JHandle::New(const Arguments& args) {
  HandleScope scope;

  assert(args.Length() == 2);
  assert(args[0]->IsExternal());

  leveldb::DB* db = (leveldb::DB*)External::Unwrap(args[0]);
  JHandle* self = new JHandle(db);

  if (args[1]->IsExternal())
    self->comparator_ = Persistent<Value>::New(args[1]);

  self->Wrap(args.This());

  return args.This();
}


/**

    Open

 */

typedef struct open_params {
  std::string name_;

  leveldb::Options options_;
  leveldb::Status status_;
  leveldb::DB* db_;

  Persistent<Value> comparator_;
  Persistent<Function> callback_;

  virtual ~open_params() {
    comparator_.Dispose();
    callback_.Dispose();
  }
} open_params_t;

static void AsyncOpen(uv_work_t* req) {
  open_params_t* op = static_cast<open_params_t*>(req->data);
  op->status_ = leveldb::DB::Open(op->options_, op->name_, &op->db_);
}

static void AfterOpen(uv_work_t* req) {
  HandleScope scope;
  open_params_t* op = static_cast<open_params_t*>(req->data);
  assert(!op->callback_.IsEmpty());

  Handle<Value> error = Null();
  Handle<Value> result = Undefined();

  if (op->status_.ok()) {
    Handle<Value> args[] = { External::New(op->db_), Undefined() };
    if (!op->comparator_.IsEmpty()) args[1] = op->comparator_;
    result = JHandle::constructor->GetFunction()->NewInstance(2, args);
  } else {
    error = Exception::Error(String::New(op->status_.ToString().c_str()));
  }

  Handle<Value> argv[] = { error, result };

  TryCatch tryCatch;
  op->callback_->Call(Context::GetCurrent()->Global(), 2, argv);
  if (tryCatch.HasCaught()) FatalException(tryCatch);

  delete op;
  delete req;
}

Handle<Value> JHandle::Open(const Arguments& args) {
  HandleScope scope;

  if (args.Length() != 3 ||
      !args[0]->IsString() ||
      !args[2]->IsFunction())
    return ThrowTypeError("Invalid arguments");

  open_params_t* op = new open_params_t;

  // Required filename
  op->name_ = *String::Utf8Value(args[0]);

  // Required callback
  op->callback_ = Persistent<Function>::New(Local<Function>::Cast(args[2]));

  // Optional options
  UnpackOptions(args[1], op->options_, &op->comparator_);

  AsyncQueue(op, AsyncOpen, AfterOpen);

  return Undefined();
}


/**

    Destroy or repair

 */

typedef struct dbop_params {
  std::string name_;

  leveldb::Options options_;
  leveldb::Status status_;

  Persistent<Function> callback_;

  virtual ~dbop_params() {
    callback_.Dispose();
  }
} dbop_params_t;

static void AfterDbOp(uv_work_t* req) {
  HandleScope scope;
  dbop_params* op = static_cast<dbop_params*>(req->data);
  assert(!op->callback_.IsEmpty());

  Handle<Value> error = Null();

  if (!op->status_.ok()) {
    error = Exception::Error(String::New(op->status_.ToString().c_str()));
  }

  Handle<Value> argv[] = { error };

  TryCatch tryCatch;
  op->callback_->Call(Context::GetCurrent()->Global(), 1, argv);
  if (tryCatch.HasCaught()) FatalException(tryCatch);

  delete op;
  delete req;
}

static Handle<Value> DbOp(const Arguments& args, const uv_work_cb async) {
  HandleScope scope;

  if (args.Length() != 3 || !args[0]->IsString())
    return ThrowTypeError("Invalid arguments");

  dbop_params_t* op = new dbop_params_t;

  // Required filename
  op->name_ = *String::Utf8Value(args[0]);

  // Optional options
  UnpackOptions(args[1], op->options_);

  // Optional callback
  if (args[2]->IsFunction())
    op->callback_ = Persistent<Function>::New(Local<Function>::Cast(args[2]));

  AsyncQueue(op, async, AfterDbOp);

  return Undefined();
}


/**

    Destroy

 */

static void AsyncDestroy(uv_work_t* req) {
  open_params_t* op = static_cast<open_params_t*>(req->data);
  op->status_ = leveldb::DestroyDB(op->name_, op->options_);
}

Handle<Value> JHandle::Destroy(const Arguments& args) {
  return DbOp(args, AsyncDestroy);
}


/**

    Repair

 */

static void AsyncRepair(uv_work_t* req) {
  open_params_t* op = static_cast<open_params_t*>(req->data);
  op->status_ = leveldb::RepairDB(op->name_, op->options_);
}

Handle<Value> JHandle::Repair(const Arguments& args) {
  return DbOp(args, AsyncRepair);
}


/**

    Read

 */

Handle<Value> JHandle::Read(const Arguments& args) {
  HandleScope scope;

  int argc = args.Length();
  if (argc < 1 || !Buffer::HasInstance(args[0]))
    return ThrowTypeError("Invalid arguments");

  ReadOp* op = ReadOp::New(Read, Read, args);
  op->key_ = ToSlice(args[0], op->keyHandle_);

  // Optional read options
  if (argc > 1) UnpackReadOptions(args[1], op->options_);

  return op->Run();
}

Handle<Value> JHandle::Write(const Arguments& args) {
  HandleScope scope;

  int argc = args.Length();
  if (argc < 1 || !JBatch::HasInstance(args[0]))
    return ThrowTypeError("Invalid arguments");

  WriteOp* op = WriteOp::New(Write, Write, args);
  op->batch_ = ObjectWrap::Unwrap<JBatch>(args[0]->ToObject());

  // Optional write options
  if (argc > 1) UnpackWriteOptions(args[1], op->options_);

  return op->Run();
}

Handle<Value> JHandle::RefIterator(leveldb::Iterator* it) {
  Local<Value> argv[] = { External::New(it) };
  Local<Object> instance = JIterator::constructor->GetFunction()->NewInstance(1, argv);

  // Keep a weak reference
  Persistent<Object> weak = Persistent<Object>::New(instance);
  weak.MakeWeak(this, &UnrefIterator);

  Ref();

  return instance;
}

void JHandle::UnrefIterator(Persistent<Value> object, void* parameter) {
  assert(object->IsObject());

  JHandle* self = static_cast<JHandle*>(parameter);
  assert(self);

  self->Unref();
  object.Dispose();
}

Handle<Value> JHandle::Iterator(const Arguments& args) {
  HandleScope scope;
  IteratorOp* op = IteratorOp::New(Iterator, Iterator, args);
  if (args.Length() > 0) UnpackReadOptions(args[0], op->options_);
  return op->Run();
}

Handle<Value> JHandle::RefSnapshot(const leveldb::Snapshot* snapshot) {
  Local<Value> instance = External::New((void*)snapshot);
  Persistent<Value> weak = Persistent<Value>::New(instance);
  weak.MakeWeak(this, &UnrefSnapshot);
  Ref();
  return instance;
}

void JHandle::UnrefSnapshot(Persistent<Value> object, void* parameter) {
  assert(object->IsExternal());

  JHandle* self = static_cast<JHandle*>(parameter);
  leveldb::Snapshot* snapshot =
    static_cast<leveldb::Snapshot*>(External::Unwrap(object));

  assert(self);
  assert(snapshot);

  self->db_->ReleaseSnapshot(snapshot);
  self->Unref();

  object.Dispose();
}

Handle<Value> JHandle::Snapshot(const Arguments& args) {
  HandleScope scope;
  return SnapshotOp::Go(Snapshot, Snapshot, args);
}

Handle<Value> JHandle::Property(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 1 || !args[0]->IsString())
    return ThrowTypeError("Invalid arguments");

  PropertyOp* op = PropertyOp::New(Property, Property, args);
  op->name_ = *String::Utf8Value(args[0]);

  return op->Run();
}

Handle<Value> JHandle::ApproximateSizes(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 1 || !args[0]->IsArray())
    return ThrowTypeError("Invalid arguments");

  Local<Array> array(Array::Cast(*args[0]));

  int len = array->Length();
  if (len % 2 != 0)
    return ThrowTypeError("Invalid arguments");

  ApproximateSizesOp* op =
    ApproximateSizesOp::New(ApproximateSizes, ApproximateSizes, args);

  for (int i = 0; i < len; i += 2) {
    if (array->Has(i) && array->Has(i + 1)) {
      Local<Value> lStart = array->Get(i);
      Local<Value> lLimit = array->Get(i + 1);

      leveldb::Slice start = ToSlice(lStart, op->handles_);
      leveldb::Slice limit = ToSlice(lLimit, op->handles_);

      op->ranges_.push_back(leveldb::Range(start, limit));
    }
  }

  return op->Run();
}

Handle<Value> JHandle::CompactRange(const Arguments& args) {
  HandleScope scope;
  return ThrowError("Method not implemented");
}


//
// ASYNC FUNCTIONS
//

void JHandle::Read(ReadOp* op) {
  op->result_ = new std::string;
  op->status_ = op->self_->db_->Get(op->options_, op->key_, op->result_);
}

void JHandle::Read(ReadOp* op, Handle<Value>& error, Handle<Value>& result) {
  if (op->status_.ok()) {
    result = ToBuffer(op->result_);
  } else if (!op->status_.IsNotFound()) {
    error = Exception::Error(String::New(op->status_.ToString().c_str()));
  }
}

void JHandle::Write(WriteOp* op) {
  op->batch_->ReadLock();
  op->status_ = op->self_->db_->Write(op->options_, &op->batch_->wb_);
  op->batch_->ReadUnlock();
}

void JHandle::Write(WriteOp* op, Handle<Value>& error, Handle<Value>& result) {
  if (!op->status_.ok())
    error = Exception::Error(String::New(op->status_.ToString().c_str()));
}

void JHandle::Iterator(IteratorOp* op) {
  op->it_ = op->self_->db_->NewIterator(op->options_);
}

void JHandle::Iterator(IteratorOp* op, Handle<Value>& error, Handle<Value>& result) {
  if (op->status_.ok()) {
    result = op->self_->RefIterator(op->it_);
  } else {
    error = Exception::Error(String::New(op->status_.ToString().c_str()));
  }
}

void JHandle::Snapshot(SnapshotOp* op) {
  op->snap_ = const_cast<leveldb::Snapshot*>(op->self_->db_->GetSnapshot());
}

void JHandle::Snapshot(SnapshotOp* op, Handle<Value>& error, Handle<Value>& result) {
  if (op->status_.ok()) {
    result = op->self_->RefSnapshot(op->snap_);
  } else {
    error = Exception::Error(String::New(op->status_.ToString().c_str()));
  }
}

void JHandle::Property(PropertyOp* op) {
  leveldb::Slice name(op->name_);
  op->hasProperty_ = op->self_->db_->GetProperty(name, &op->value_);
}

void JHandle::Property(PropertyOp* op, Handle<Value>& error, Handle<Value>& result) {
  if (op->hasProperty_) result = String::New(op->value_.c_str());
}

void JHandle::ApproximateSizes(ApproximateSizesOp* op) {
  int len = op->ranges_.size();
  op->sizes_ = new uint64_t[len];
  op->self_->db_->GetApproximateSizes(&op->ranges_[0], len, op->sizes_);
}

void JHandle::ApproximateSizes(ApproximateSizesOp* op,
                               Handle<Value>& error, Handle<Value>& result)
{
  int len = op->ranges_.size();

  Handle<Array> array = Array::New(len);

  for (int i = 0; i < len; ++i) {
    uint64_t size = op->sizes_[i];
    if (size < INT_MAX) {
      array->Set(i, Integer::New(static_cast<uint32_t>(size)));
    } else {
      array->Set(i, Number::New(static_cast<double>(size)));
    }
  }

  result = array;
}

} // namespace node_leveldb
