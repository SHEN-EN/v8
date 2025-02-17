// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/web-snapshot/web-snapshot.h"

#include <limits>

#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-object.h"
#include "include/v8-primitive.h"
#include "include/v8-script.h"
#include "src/api/api-inl.h"
#include "src/base/platform/wrappers.h"
#include "src/handles/handles.h"
#include "src/logging/runtime-call-stats-scope.h"
#include "src/objects/contexts.h"
#include "src/objects/js-regexp-inl.h"
#include "src/objects/script.h"

namespace v8 {
namespace internal {

constexpr uint8_t WebSnapshotSerializerDeserializer::kMagicNumber[4];

// When encountering an error during deserializing, we note down the error but
// don't bail out from processing the snapshot further. This is to speed up
// deserialization; the error case is now slower since we don't bail out, but
// the non-error case is faster, since we don't repeatedly check for errors.
// (Invariant: we might fill our internal data structures with arbitrary data,
// but it shouldn't have an observable effect.)

// This doesn't increase the complexity of processing the data in a robust and
// secure way. We cannot trust the data anyway, so every upcoming byte can have
// an arbitrary value, not depending on whether or not we've encountered an
// error before.
void WebSnapshotSerializerDeserializer::Throw(const char* message) {
  if (error_message_ != nullptr) {
    return;
  }
  error_message_ = message;
  if (!isolate_->has_pending_exception()) {
    isolate_->Throw(*isolate_->factory()->NewError(
        MessageTemplate::kWebSnapshotError,
        isolate_->factory()->NewStringFromAsciiChecked(error_message_)));
  }
}

uint32_t WebSnapshotSerializerDeserializer::FunctionKindToFunctionFlags(
    FunctionKind kind) {
  // TODO(v8:11525): Support more function kinds.
  switch (kind) {
    case FunctionKind::kNormalFunction:
    case FunctionKind::kArrowFunction:
    case FunctionKind::kGeneratorFunction:
    case FunctionKind::kAsyncFunction:
    case FunctionKind::kAsyncArrowFunction:
    case FunctionKind::kAsyncGeneratorFunction:
    case FunctionKind::kBaseConstructor:
    case FunctionKind::kDefaultBaseConstructor:
    case FunctionKind::kConciseMethod:
    case FunctionKind::kAsyncConciseMethod:
      break;
    default:
      Throw("Unsupported function kind");
  }
  auto flags = AsyncFunctionBitField::encode(IsAsyncFunction(kind)) |
               GeneratorFunctionBitField::encode(IsGeneratorFunction(kind)) |
               ArrowFunctionBitField::encode(IsArrowFunction(kind)) |
               MethodBitField::encode(IsConciseMethod(kind)) |
               StaticBitField::encode(IsStatic(kind)) |
               ClassConstructorBitField::encode(IsClassConstructor(kind)) |
               DefaultConstructorBitField::encode(IsDefaultConstructor(kind)) |
               DerivedConstructorBitField::encode(IsDerivedConstructor(kind));
  return flags;
}

// TODO(v8:11525): Optionally, use an enum instead.
FunctionKind WebSnapshotSerializerDeserializer::FunctionFlagsToFunctionKind(
    uint32_t flags) {
  FunctionKind kind;
  if (IsFunctionOrMethod(flags)) {
    if (ArrowFunctionBitField::decode(flags) && MethodBitField::decode(flags)) {
      kind = FunctionKind::kInvalid;
    } else {
      uint32_t index = AsyncFunctionBitField::decode(flags) << 0 |
                       GeneratorFunctionBitField::decode(flags) << 1 |
                       (ArrowFunctionBitField::decode(flags) ||
                        StaticBitField::decode(flags))
                           << 2 |
                       MethodBitField::decode(flags) << 3;
      static const FunctionKind kFunctionKinds[] = {
          // kNormalFunction
          // is_generator = false
          FunctionKind::kNormalFunction,  // is_async = false
          FunctionKind::kAsyncFunction,   // is_async = true
          // is_generator = true
          FunctionKind::kGeneratorFunction,       // is_async = false
          FunctionKind::kAsyncGeneratorFunction,  // is_async = true

          // kArrowFunction
          // is_generator = false
          FunctionKind::kArrowFunction,       // is_async = false
          FunctionKind::kAsyncArrowFunction,  // is_async = true
          // is_generator = true
          FunctionKind::kInvalid,  // is_async = false
          FunctionKind::kInvalid,  // is_async = true

          // kNonStaticMethod
          // is_generator = false
          FunctionKind::kConciseMethod,       // is_async = false
          FunctionKind::kAsyncConciseMethod,  // is_async = true
          // is_generator = true
          // TODO(v8::11525) Support FunctionKind::kConciseGeneratorMethod.
          FunctionKind::kInvalid,  // is_async = false
          // TODO(v8::11525) Support FunctionKind::kAsyncConciseGeneratorMethod.
          FunctionKind::kInvalid,  // is_async = true

          // kStaticMethod
          // is_generator = false
          // TODO(v8::11525) Support FunctionKind::kStaticConciseMethod.
          FunctionKind::kInvalid,  // is_async = false
          // TODO(v8::11525) Support FunctionKind::kStaticAsyncConciseMethod.
          FunctionKind::kInvalid,  // is_async = true
          // is_generator = true
          // TODO(v8::11525) Support
          // FunctionKind::kStaticConciseGeneratorMethod.
          FunctionKind::kInvalid,  // is_async = false
          // TODO(v8::11525) Support
          // FunctionKind::kStaticAsyncConciseGeneratorMethod.
          FunctionKind::kInvalid  // is_async = true
      };
      kind = kFunctionKinds[index];
    }
  } else if (IsConstructor(flags)) {
    static const FunctionKind kFunctionKinds[] = {
        // is_derived = false
        FunctionKind::kBaseConstructor,         // is_default = false
        FunctionKind::kDefaultBaseConstructor,  // is_default = true
        // is_derived = true
        FunctionKind::kDerivedConstructor,        // is_default = false
        FunctionKind::kDefaultDerivedConstructor  // is_default = true
    };
    kind = kFunctionKinds[flags >> DefaultConstructorBitField::kShift];
  } else {
    kind = FunctionKind::kInvalid;
  }
  if (kind == FunctionKind::kInvalid) {
    Throw("Invalid function flags\n");
  }
  return kind;
}

bool WebSnapshotSerializerDeserializer::IsFunctionOrMethod(uint32_t flags) {
  uint32_t mask = AsyncFunctionBitField::kMask |
                  GeneratorFunctionBitField::kMask |
                  ArrowFunctionBitField::kMask | MethodBitField::kMask |
                  StaticBitField::kMask;
  return (flags & mask) == flags;
}

bool WebSnapshotSerializerDeserializer::IsConstructor(uint32_t flags) {
  uint32_t mask = ClassConstructorBitField::kMask |
                  DefaultConstructorBitField::kMask |
                  DerivedConstructorBitField::kMask;
  return ClassConstructorBitField::decode(flags) && (flags & mask) == flags;
}

uint32_t WebSnapshotSerializerDeserializer::GetDefaultAttributeFlags() {
  auto flags = ReadOnlyBitField::encode(false) |
               ConfigurableBitField::encode(true) |
               EnumerableBitField::encode(true);
  return flags;
}

uint32_t WebSnapshotSerializerDeserializer::AttributesToFlags(
    PropertyDetails details) {
  auto flags = ReadOnlyBitField::encode(details.IsReadOnly()) |
               ConfigurableBitField::encode(details.IsConfigurable()) |
               EnumerableBitField::encode(details.IsEnumerable());
  return flags;
}

PropertyAttributes WebSnapshotSerializerDeserializer::FlagsToAttributes(
    uint32_t flags) {
  uint32_t attributes = ReadOnlyBitField::decode(flags) * READ_ONLY +
                        !ConfigurableBitField::decode(flags) * DONT_DELETE +
                        !EnumerableBitField::decode(flags) * DONT_ENUM;
  return static_cast<PropertyAttributes>(attributes);
}

WebSnapshotSerializer::WebSnapshotSerializer(v8::Isolate* isolate)
    : WebSnapshotSerializerDeserializer(
          reinterpret_cast<v8::internal::Isolate*>(isolate)),
      string_serializer_(isolate_, nullptr),
      map_serializer_(isolate_, nullptr),
      context_serializer_(isolate_, nullptr),
      function_serializer_(isolate_, nullptr),
      class_serializer_(isolate_, nullptr),
      array_serializer_(isolate_, nullptr),
      object_serializer_(isolate_, nullptr),
      export_serializer_(isolate_, nullptr),
      string_ids_(isolate_->heap()),
      map_ids_(isolate_->heap()),
      context_ids_(isolate_->heap()),
      function_ids_(isolate_->heap()),
      class_ids_(isolate_->heap()),
      array_ids_(isolate_->heap()),
      object_ids_(isolate_->heap()) {}

WebSnapshotSerializer::~WebSnapshotSerializer() {}

bool WebSnapshotSerializer::TakeSnapshot(v8::Local<v8::Context> context,
                                         v8::Local<v8::PrimitiveArray> exports,
                                         WebSnapshotData& data_out) {
  if (string_ids_.size() > 0) {
    Throw("Can't reuse WebSnapshotSerializer");
    return false;
  }
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate_);

  contexts_ = ArrayList::New(isolate_, 30);
  functions_ = ArrayList::New(isolate_, 30);
  classes_ = ArrayList::New(isolate_, 30);
  arrays_ = ArrayList::New(isolate_, 30);
  objects_ = ArrayList::New(isolate_, 30);

  std::unique_ptr<Handle<JSObject>[]> export_objects(
      new Handle<JSObject>[exports->Length()]);
  for (int i = 0, length = exports->Length(); i < length; ++i) {
    v8::Local<v8::String> str =
        exports->Get(v8_isolate, i)->ToString(context).ToLocalChecked();
    if (str.IsEmpty()) {
      continue;
    }
    v8::ScriptCompiler::Source source(str);
    auto script = ScriptCompiler::Compile(context, &source).ToLocalChecked();
    v8::MaybeLocal<v8::Value> script_result = script->Run(context);
    v8::Local<v8::Object> v8_object;
    if (script_result.IsEmpty() ||
        !script_result.ToLocalChecked()->ToObject(context).ToLocal(
            &v8_object)) {
      Throw("Exported object not found");
      return false;
    }

    export_objects[i] = Handle<JSObject>::cast(Utils::OpenHandle(*v8_object));
    Discovery(export_objects[i]);
  }

  SerializeSource();

  for (int i = 0, length = exports->Length(); i < length; ++i) {
    v8::Local<v8::String> str =
        exports->Get(v8_isolate, i)->ToString(context).ToLocalChecked();
    SerializeExport(export_objects[i],
                    Handle<String>::cast(Utils::OpenHandle(*str)));
  }

  WriteSnapshot(data_out.buffer, data_out.buffer_size);

  if (has_error()) {
    isolate_->ReportPendingMessages();
    return false;
  }
  return true;
}

void WebSnapshotSerializer::SerializePendingItems() {
  for (int i = 0; i < contexts_->Length(); ++i) {
    Handle<Context> context =
        handle(Context::cast(contexts_->Get(i)), isolate_);
    SerializeContext(context);
  }
  for (int i = 0; i < functions_->Length(); ++i) {
    Handle<JSFunction> function =
        handle(JSFunction::cast(functions_->Get(i)), isolate_);
    SerializeFunction(function);
  }
  for (int i = 0; i < classes_->Length(); ++i) {
    Handle<JSFunction> function =
        handle(JSFunction::cast(classes_->Get(i)), isolate_);
    SerializeClass(function);
  }
  for (int i = 0; i < arrays_->Length(); ++i) {
    Handle<JSArray> array = handle(JSArray::cast(arrays_->Get(i)), isolate_);
    SerializeArray(array);
  }
  for (int i = 0; i < objects_->Length(); ++i) {
    Handle<JSObject> object =
        handle(JSObject::cast(objects_->Get(i)), isolate_);
    SerializeObject(object);
  }
  // Maps and strings get serialized when they're encountered; we don't need to
  // serialize them explicitly.
}

// Format (full snapshot):
// - Magic number (4 bytes)
// - String count
// - For each string:
//   - Serialized string
// - Shape count
// - For each shape:
//   - Serialized shape
// - Context count
// - For each context:
//   - Serialized context
// - Function count
// - For each function:
//   - Serialized function
// - Object count
// - For each object:
//   - Serialized object
// - Export count
// - For each export:
//   - Serialized export
void WebSnapshotSerializer::WriteSnapshot(uint8_t*& buffer,
                                          size_t& buffer_size) {
  SerializePendingItems();

  ValueSerializer total_serializer(isolate_, nullptr);
  size_t needed_size =
      sizeof(kMagicNumber) + string_serializer_.buffer_size_ +
      map_serializer_.buffer_size_ + context_serializer_.buffer_size_ +
      function_serializer_.buffer_size_ + class_serializer_.buffer_size_ +
      array_serializer_.buffer_size_ + object_serializer_.buffer_size_ +
      export_serializer_.buffer_size_ + 8 * sizeof(uint32_t);
  if (total_serializer.ExpandBuffer(needed_size).IsNothing()) {
    Throw("Out of memory");
    return;
  }
  total_serializer.WriteRawBytes(kMagicNumber, 4);
  total_serializer.WriteUint32(static_cast<uint32_t>(string_count()));
  total_serializer.WriteRawBytes(string_serializer_.buffer_,
                                 string_serializer_.buffer_size_);
  total_serializer.WriteUint32(static_cast<uint32_t>(map_count()));
  total_serializer.WriteRawBytes(map_serializer_.buffer_,
                                 map_serializer_.buffer_size_);
  total_serializer.WriteUint32(static_cast<uint32_t>(context_count()));
  total_serializer.WriteRawBytes(context_serializer_.buffer_,
                                 context_serializer_.buffer_size_);
  total_serializer.WriteUint32(static_cast<uint32_t>(function_count()));
  total_serializer.WriteRawBytes(function_serializer_.buffer_,
                                 function_serializer_.buffer_size_);
  total_serializer.WriteUint32(static_cast<uint32_t>(array_count()));
  total_serializer.WriteRawBytes(array_serializer_.buffer_,
                                 array_serializer_.buffer_size_);
  total_serializer.WriteUint32(static_cast<uint32_t>(object_count()));
  total_serializer.WriteRawBytes(object_serializer_.buffer_,
                                 object_serializer_.buffer_size_);
  total_serializer.WriteUint32(static_cast<uint32_t>(class_count()));
  total_serializer.WriteRawBytes(class_serializer_.buffer_,
                                 class_serializer_.buffer_size_);
  total_serializer.WriteUint32(export_count_);
  total_serializer.WriteRawBytes(export_serializer_.buffer_,
                                 export_serializer_.buffer_size_);

  if (has_error()) {
    return;
  }

  auto result = total_serializer.Release();
  buffer = result.first;
  buffer_size = result.second;
}

bool WebSnapshotSerializer::InsertIntoIndexMap(ObjectCacheIndexMap& map,
                                               Handle<HeapObject> object,
                                               uint32_t& id) {
  if (static_cast<uint32_t>(map.size()) >=
      std::numeric_limits<uint32_t>::max()) {
    Throw("Too many objects");
    return true;
  }
  int index_out;
  bool found = map.LookupOrInsert(object, &index_out);
  id = static_cast<uint32_t>(index_out);
  return found;
}

// Format:
// - Length
// - Raw bytes (data)
void WebSnapshotSerializer::SerializeString(Handle<String> string,
                                            uint32_t& id) {
  if (InsertIntoIndexMap(string_ids_, string, id)) {
    return;
  }

  // TODO(v8:11525): Always write strings as UTF-8.
  string = String::Flatten(isolate_, string);
  DisallowGarbageCollection no_gc;
  String::FlatContent flat = string->GetFlatContent(no_gc);
  DCHECK(flat.IsFlat());
  if (flat.IsOneByte()) {
    base::Vector<const uint8_t> chars = flat.ToOneByteVector();
    string_serializer_.WriteUint32(chars.length());
    string_serializer_.WriteRawBytes(chars.begin(),
                                     chars.length() * sizeof(uint8_t));
  } else if (flat.IsTwoByte()) {
    v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate_);
    v8::Local<v8::String> api_string = Utils::ToLocal(string);
    int length = api_string->Utf8Length(v8_isolate);
    std::unique_ptr<char[]> buffer(new char[length]);
    api_string->WriteUtf8(v8_isolate, buffer.get(), length);
    string_serializer_.WriteUint32(length);
    string_serializer_.WriteRawBytes(buffer.get(), length * sizeof(uint8_t));
  } else {
    UNREACHABLE();
  }
}

// Format (serialized shape):
// - PropertyAttributesType
// - 0 if the __proto__ is Object.prototype, 1 + object id for the __proto__
//   otherwise
// - Property count
// - For each property
//   - String id (name)
//   - If the PropertyAttributesType is CUSTOM: attributes
void WebSnapshotSerializer::SerializeMap(Handle<Map> map, uint32_t& id) {
  if (InsertIntoIndexMap(map_ids_, map, id)) {
    return;
  }

  int first_custom_index = -1;
  std::vector<uint32_t> string_ids;
  std::vector<uint32_t> attributes;
  string_ids.reserve(map->NumberOfOwnDescriptors());
  attributes.reserve(map->NumberOfOwnDescriptors());
  for (InternalIndex i : map->IterateOwnDescriptors()) {
    Handle<Name> key(map->instance_descriptors(kRelaxedLoad).GetKey(i),
                     isolate_);
    if (!key->IsString()) {
      Throw("Key is not a string");
      return;
    }

    PropertyDetails details =
        map->instance_descriptors(kRelaxedLoad).GetDetails(i);

    if (details.location() != PropertyLocation::kField) {
      Throw("Properties which are not fields not supported");
      return;
    }
    if (first_custom_index >= 0 || details.IsReadOnly() ||
        !details.IsConfigurable() || details.IsDontEnum()) {
      if (first_custom_index == -1) first_custom_index = i.as_int();
      attributes.push_back(AttributesToFlags(details));
    }

    uint32_t string_id = 0;
    SerializeString(Handle<String>::cast(key), string_id);
    string_ids.push_back(string_id);
  }

  map_serializer_.WriteUint32(first_custom_index == -1
                                  ? PropertyAttributesType::DEFAULT
                                  : PropertyAttributesType::CUSTOM);

  if (map->prototype() == isolate_->context().initial_object_prototype()) {
    map_serializer_.WriteUint32(0);
  } else {
    // TODO(v8:11525): Support non-JSObject prototypes, at least null. Recognize
    // well-known objects to that we don't end up encoding them in the snapshot.
    if (!map->prototype().IsJSObject()) {
      Throw("Non-JSObject __proto__s not supported");
      return;
    }
    uint32_t prototype_id = GetObjectId(JSObject::cast(map->prototype()));
    map_serializer_.WriteUint32(prototype_id + 1);
  }

  map_serializer_.WriteUint32(static_cast<uint32_t>(string_ids.size()));

  uint32_t default_flags = GetDefaultAttributeFlags();
  for (size_t i = 0; i < string_ids.size(); ++i) {
    if (first_custom_index >= 0) {
      if (static_cast<int>(i) < first_custom_index) {
        map_serializer_.WriteUint32(default_flags);
      } else {
        map_serializer_.WriteUint32(attributes[i - first_custom_index]);
      }
    }
    map_serializer_.WriteUint32(string_ids[i]);
  }
}

// Construct the minimal source string to be included in the snapshot. Maintain
// the "inner function is textually inside its outer function" relationship.
// Example:
// Input:
// Full source:       abcdefghijklmnopqrstuvwxyzåäö
// Functions:            11111111       22222222  3
// Inner functions:       44  55         666
// Output:
// Constructed source:   defghijkstuvwxyzö
// Functions:            11111111222222223
// Inner functions        44  55  666
void WebSnapshotSerializer::SerializeSource() {
  if (source_intervals_.empty()) {
    return;
  }

  Factory* factory = isolate_->factory();
  Handle<String> source_string = factory->empty_string();
  int current_interval_start = 0;
  int current_interval_end = 0;
  for (const auto& interval : source_intervals_) {
    DCHECK_LE(current_interval_start, interval.first);  // Iterated in order.
    DCHECK_LE(interval.first, interval.second);
    if (interval.second <= current_interval_end) {
      // This interval is fully within the current interval. We don't need to
      // include any new source code, just record the position conversion.
      auto offset_within_parent = interval.first - current_interval_start;
      source_offset_to_compacted_source_offset_[interval.first] =
          source_offset_to_compacted_source_offset_[current_interval_start] +
          offset_within_parent;
      continue;
    }
    // Start a new interval.
    current_interval_start = interval.first;
    current_interval_end = interval.second;
    source_offset_to_compacted_source_offset_[current_interval_start] =
        source_string->length();
    MaybeHandle<String> new_source_string = factory->NewConsString(
        source_string,
        factory->NewSubString(full_source_, current_interval_start,
                              current_interval_end));
    if (!new_source_string.ToHandle(&source_string)) {
      Throw("Cannot construct source string");
      return;
    }
  }
  SerializeString(source_string, source_id_);
}

void WebSnapshotSerializer::SerializeFunctionInfo(ValueSerializer* serializer,
                                                  Handle<JSFunction> function) {
  if (!function->shared().HasSourceCode()) {
    Throw("Function without source code");
    return;
  }

  {
    DisallowGarbageCollection no_gc;
    Context context = function->context();
    if (context.IsNativeContext() || context.IsScriptContext()) {
      serializer->WriteUint32(0);
    } else {
      DCHECK(context.IsFunctionContext() || context.IsBlockContext());
      uint32_t context_id = GetContextId(context);
      serializer->WriteUint32(context_id + 1);
    }
  }

  DCHECK_EQ(source_id_, 0);
  serializer->WriteUint32(source_id_);
  int start = function->shared().StartPosition();
  int end = function->shared().EndPosition();
  serializer->WriteUint32(source_offset_to_compacted_source_offset_[start]);
  serializer->WriteUint32(end - start);

  serializer->WriteUint32(
      function->shared().internal_formal_parameter_count_without_receiver());
  serializer->WriteUint32(
      FunctionKindToFunctionFlags(function->shared().kind()));

  if (function->has_prototype_slot() && function->has_instance_prototype()) {
    DisallowGarbageCollection no_gc;
    JSObject prototype = JSObject::cast(function->instance_prototype());
    uint32_t prototype_id = GetObjectId(prototype);
    serializer->WriteUint32(prototype_id + 1);
  } else {
    serializer->WriteUint32(0);
  }
}

void WebSnapshotSerializer::Discovery(Handle<Object> start_object) {
  // The object discovery phase assigns IDs for objects / functions / classes /
  // arrays and discovers outgoing references from them. This is needed so that
  // e.g., we know all functions upfront and can construct the source code that
  // covers them before serializing the functions.

  // TODO(v8:11525): Serialize leaf objects first.

  discovery_queue_.push(start_object);

  while (!discovery_queue_.empty()) {
    const Handle<Object>& object = discovery_queue_.front();
    if (object->IsHeapObject()) {
      switch (HeapObject::cast(*object).map().instance_type()) {
        case JS_FUNCTION_TYPE:
          DiscoverFunction(Handle<JSFunction>::cast(object));
          break;
        case JS_CLASS_CONSTRUCTOR_TYPE:
          DiscoverClass(Handle<JSFunction>::cast(object));
          break;
        case JS_OBJECT_TYPE:
          DiscoverObject(Handle<JSObject>::cast(object));
          break;
        case JS_ARRAY_TYPE:
          DiscoverArray(Handle<JSArray>::cast(object));
          break;
        case ODDBALL_TYPE:
        case HEAP_NUMBER_TYPE:
        case JS_PRIMITIVE_WRAPPER_TYPE:
        case JS_REG_EXP_TYPE:
          // Can't contain references to other objects.
          break;
        default:
          if (object->IsString()) {
            // Can't contain references to other objects.
            break;
          } else {
            Throw("Unsupported object");
          }
      }
    }
    discovery_queue_.pop();
  }
}

void WebSnapshotSerializer::DiscoverFunction(Handle<JSFunction> function) {
  uint32_t id;
  if (InsertIntoIndexMap(function_ids_, function, id)) {
    return;
  }

  DCHECK_EQ(id, functions_->Length());
  functions_ = ArrayList::Add(isolate_, functions_, function);
  DiscoverContextAndPrototype(function);
  // TODO(v8:11525): Support properties in functions.
  DiscoverSource(function);
}

void WebSnapshotSerializer::DiscoverClass(Handle<JSFunction> function) {
  uint32_t id;
  if (InsertIntoIndexMap(class_ids_, function, id)) {
    return;
  }

  DCHECK_EQ(id, classes_->Length());
  classes_ = ArrayList::Add(isolate_, classes_, function);

  DiscoverContextAndPrototype(function);
  // TODO(v8:11525): Support properties in classes.
  // TODO(v8:11525): Support class members.
  DiscoverSource(function);
}

void WebSnapshotSerializer::DiscoverContextAndPrototype(
    Handle<JSFunction> function) {
  Handle<Context> context(function->context(), isolate_);
  if (context->IsFunctionContext() || context->IsBlockContext()) {
    DiscoverContext(context);
  }

  if (function->has_prototype_slot() &&
      function->map().has_non_instance_prototype()) {
    Throw("Functions with non-instance prototypes not supported");
    return;
  }

  if (function->has_prototype_slot() && function->has_instance_prototype()) {
    Handle<JSObject> prototype = Handle<JSObject>::cast(
        handle(function->instance_prototype(), isolate_));
    discovery_queue_.push(prototype);
  }
}

void WebSnapshotSerializer::DiscoverContext(Handle<Context> context) {
  // Ensure that parent contexts get a lower ID.
  if (!context->previous().IsNativeContext() &&
      !context->previous().IsScriptContext()) {
    DiscoverContext(handle(context->previous(), isolate_));
  }

  uint32_t id;
  if (InsertIntoIndexMap(context_ids_, context, id)) {
    return;
  }

  DCHECK_EQ(id, contexts_->Length());
  contexts_ = ArrayList::Add(isolate_, contexts_, context);

  Handle<ScopeInfo> scope_info(context->scope_info(), isolate_);
  int count = scope_info->ContextLocalCount();

  for (int i = 0; i < count; ++i) {
    // TODO(v8:11525): support parameters
    // TODO(v8:11525): distinguish variable modes
    Handle<Object> value(context->get(scope_info->ContextHeaderLength() + i),
                         isolate_);
    discovery_queue_.push(value);
  }
}

void WebSnapshotSerializer::DiscoverSource(Handle<JSFunction> function) {
  source_intervals_.emplace(function->shared().StartPosition(),
                            function->shared().EndPosition());
  Handle<String> function_script_source =
      handle(String::cast(Script::cast(function->shared().script()).source()),
             isolate_);
  if (full_source_.is_null()) {
    full_source_ = function_script_source;
  } else if (!full_source_->Equals(*function_script_source)) {
    Throw("Cannot include functions from multiple scripts");
  }
}

void WebSnapshotSerializer::DiscoverArray(Handle<JSArray> array) {
  uint32_t id;
  if (InsertIntoIndexMap(array_ids_, array, id)) {
    return;
  }

  DCHECK_EQ(id, arrays_->Length());
  arrays_ = ArrayList::Add(isolate_, arrays_, array);

  auto elements_kind = array->GetElementsKind();
  if (elements_kind != PACKED_SMI_ELEMENTS &&
      elements_kind != PACKED_ELEMENTS) {
    Throw("Unsupported array");
    return;
  }
  // TODO(v8:11525): Support sparse arrays & arrays with holes.
  uint32_t length = static_cast<uint32_t>(array->length().ToSmi().value());
  Handle<FixedArray> elements =
      handle(FixedArray::cast(array->elements()), isolate_);
  for (uint32_t i = 0; i < length; ++i) {
    discovery_queue_.push(handle(elements->get(i), isolate_));
  }
}

void WebSnapshotSerializer::DiscoverObject(Handle<JSObject> object) {
  uint32_t id;
  if (InsertIntoIndexMap(object_ids_, object, id)) {
    return;
  }

  DCHECK_EQ(id, objects_->Length());
  objects_ = ArrayList::Add(isolate_, objects_, object);

  // TODO(v8:11525): Support objects with so many properties that they can't be
  // in fast mode.
  JSObject::MigrateSlowToFast(object, 0, "Web snapshot");
  if (!object->HasFastProperties()) {
    Throw("Dictionary mode objects not supported");
  }

  Handle<Map> map(object->map(), isolate_);

  // Discover __proto__.
  if (map->prototype() != isolate_->context().initial_object_prototype()) {
    discovery_queue_.push(handle(map->prototype(), isolate_));
  }

  // Discover property values.
  for (InternalIndex i : map->IterateOwnDescriptors()) {
    PropertyDetails details =
        map->instance_descriptors(kRelaxedLoad).GetDetails(i);
    FieldIndex field_index = FieldIndex::ForDescriptor(*map, i);
    Handle<Object> value = JSObject::FastPropertyAt(
        isolate_, object, details.representation(), field_index);
    discovery_queue_.push(value);
  }
}

// Format (serialized function):
// - 0 if there's no context, 1 + context id otherwise
// - String id (source snippet)
// - Start position in the source snippet
// - Length in the source snippet
// - Formal parameter count
// - Flags (see FunctionFlags)
// - 0 if there's no function prototype, 1 + object id for the function
// prototype otherwise
// TODO(v8:11525): Investigate whether the length is really needed.
void WebSnapshotSerializer::SerializeFunction(Handle<JSFunction> function) {
  SerializeFunctionInfo(&function_serializer_, function);
  // TODO(v8:11525): Support properties in functions.
}

// Format (serialized class):
// - 1 + context id
// - String id (source snippet)
// - Start position in the source snippet
// - Length in the source snippet
// - Formal parameter count
// - Flags (see FunctionFlags)
// - 1 + object id for the function prototype
void WebSnapshotSerializer::SerializeClass(Handle<JSFunction> function) {
  SerializeFunctionInfo(&class_serializer_, function);
  // TODO(v8:11525): Support properties in classes.
  // TODO(v8:11525): Support class members.
}

// Format (serialized context):
// - 0 if there's no parent context, 1 + parent context id otherwise
// - Variable count
// - For each variable:
//   - String id (name)
//   - Serialized value
void WebSnapshotSerializer::SerializeContext(Handle<Context> context) {
  uint32_t parent_context_id = 0;
  if (!context->previous().IsNativeContext() &&
      !context->previous().IsScriptContext()) {
    parent_context_id = GetContextId(context->previous()) + 1;
  }

  // TODO(v8:11525): Use less space for encoding the context type.
  if (context->IsFunctionContext()) {
    context_serializer_.WriteUint32(ContextType::FUNCTION);
  } else if (context->IsBlockContext()) {
    context_serializer_.WriteUint32(ContextType::BLOCK);
  } else {
    Throw("Unsupported context type");
    return;
  }

  context_serializer_.WriteUint32(parent_context_id);

  Handle<ScopeInfo> scope_info(context->scope_info(), isolate_);
  int count = scope_info->ContextLocalCount();
  context_serializer_.WriteUint32(count);

  for (int i = 0; i < count; ++i) {
    // TODO(v8:11525): support parameters
    // TODO(v8:11525): distinguish variable modes
    Handle<String> name(scope_info->context_local_names(i), isolate_);
    uint32_t string_id = 0;
    SerializeString(name, string_id);
    context_serializer_.WriteUint32(string_id);
    Handle<Object> value(context->get(scope_info->ContextHeaderLength() + i),
                         isolate_);
    WriteValue(value, context_serializer_);
  }
}

// Format (serialized object):
// - Shape id
// - For each property:
//   - Serialized value
void WebSnapshotSerializer::SerializeObject(Handle<JSObject> object) {
  Handle<Map> map(object->map(), isolate_);
  uint32_t map_id = 0;
  SerializeMap(map, map_id);

  if (*map != object->map()) {
    Throw("Map changed");
    return;
  }

  object_serializer_.WriteUint32(map_id);

  for (InternalIndex i : map->IterateOwnDescriptors()) {
    PropertyDetails details =
        map->instance_descriptors(kRelaxedLoad).GetDetails(i);
    FieldIndex field_index = FieldIndex::ForDescriptor(*map, i);
    Handle<Object> value = JSObject::FastPropertyAt(
        isolate_, object, details.representation(), field_index);
    WriteValue(value, object_serializer_);
  }
}

// Format (serialized array):
// - Length
// - For each element:
//   - Serialized value
void WebSnapshotSerializer::SerializeArray(Handle<JSArray> array) {
  auto elements_kind = array->GetElementsKind();
  if (elements_kind != PACKED_SMI_ELEMENTS &&
      elements_kind != PACKED_ELEMENTS) {
    Throw("Unsupported array");
    return;
  }
  // TODO(v8:11525): Support sparse arrays & arrays with holes.
  uint32_t length = static_cast<uint32_t>(array->length().ToSmi().value());
  array_serializer_.WriteUint32(length);
  Handle<FixedArray> elements =
      handle(FixedArray::cast(array->elements()), isolate_);
  for (uint32_t i = 0; i < length; ++i) {
    WriteValue(handle(elements->get(i), isolate_), array_serializer_);
  }
}

// Format (serialized export):
// - String id (export name)
// - Serialized value (export value)
void WebSnapshotSerializer::SerializeExport(Handle<JSObject> object,
                                            Handle<String> export_name) {
  ++export_count_;
  uint32_t string_id = 0;
  SerializeString(export_name, string_id);
  export_serializer_.WriteUint32(string_id);
  if (object->IsJSPrimitiveWrapper()) {
    Handle<JSPrimitiveWrapper> wrapper =
        Handle<JSPrimitiveWrapper>::cast(object);
    Handle<Object> export_value =
        handle(JSPrimitiveWrapper::cast(*wrapper).value(), isolate_);
    WriteValue(export_value, export_serializer_);
  } else {
    WriteValue(object, export_serializer_);
  }
}

// Format (serialized value):
// - Type id (ValueType enum)
// - Value or id (interpretation depends on the type)
void WebSnapshotSerializer::WriteValue(Handle<Object> object,
                                       ValueSerializer& serializer) {
  uint32_t id = 0;
  if (object->IsSmi()) {
    serializer.WriteUint32(ValueType::INTEGER);
    serializer.WriteZigZag<int32_t>(Smi::cast(*object).value());
    return;
  }

  DCHECK(object->IsHeapObject());
  switch (HeapObject::cast(*object).map().instance_type()) {
    case ODDBALL_TYPE:
      switch (Oddball::cast(*object).kind()) {
        case Oddball::kFalse:
          serializer.WriteUint32(ValueType::FALSE_CONSTANT);
          return;
        case Oddball::kTrue:
          serializer.WriteUint32(ValueType::TRUE_CONSTANT);
          return;
        case Oddball::kNull:
          serializer.WriteUint32(ValueType::NULL_CONSTANT);
          return;
        case Oddball::kUndefined:
          serializer.WriteUint32(ValueType::UNDEFINED_CONSTANT);
          return;
        default:
          UNREACHABLE();
      }
    case HEAP_NUMBER_TYPE:
      // TODO(v8:11525): Handle possible endianness mismatch.
      serializer.WriteUint32(ValueType::DOUBLE);
      serializer.WriteDouble(HeapNumber::cast(*object).value());
      break;
    case JS_FUNCTION_TYPE:
      serializer.WriteUint32(ValueType::FUNCTION_ID);
      serializer.WriteUint32(GetFunctionId(JSFunction::cast(*object)));
      break;
    case JS_CLASS_CONSTRUCTOR_TYPE:
      serializer.WriteUint32(ValueType::CLASS_ID);
      serializer.WriteUint32(GetClassId(JSFunction::cast(*object)));
      break;
    case JS_OBJECT_TYPE:
      serializer.WriteUint32(ValueType::OBJECT_ID);
      serializer.WriteUint32(GetObjectId(JSObject::cast(*object)));
      break;
    case JS_ARRAY_TYPE:
      serializer.WriteUint32(ValueType::ARRAY_ID);
      serializer.WriteUint32(GetArrayId(JSArray::cast(*object)));
      break;
    case JS_REG_EXP_TYPE: {
      Handle<JSRegExp> regexp = Handle<JSRegExp>::cast(object);
      if (regexp->map() != isolate_->regexp_function()->initial_map()) {
        Throw("Unsupported RegExp map");
        return;
      }
      uint32_t pattern_id, flags_id;
      Handle<String> pattern = handle(regexp->source(), isolate_);
      Handle<String> flags_string =
          JSRegExp::StringFromFlags(isolate_, regexp->flags());
      SerializeString(pattern, pattern_id);
      SerializeString(flags_string, flags_id);
      serializer.WriteUint32(ValueType::REGEXP);
      serializer.WriteUint32(pattern_id);
      serializer.WriteUint32(flags_id);
      break;
    }
    default:
      if (object->IsString()) {
        SerializeString(Handle<String>::cast(object), id);
        serializer.WriteUint32(ValueType::STRING_ID);
        serializer.WriteUint32(id);
      } else {
        Throw("Unsupported object");
      }
  }
  // TODO(v8:11525): Support more types.
}

uint32_t WebSnapshotSerializer::GetFunctionId(JSFunction function) {
  int id;
  bool return_value = function_ids_.Lookup(function, &id);
  DCHECK(return_value);
  USE(return_value);
  return static_cast<uint32_t>(id);
}

uint32_t WebSnapshotSerializer::GetClassId(JSFunction function) {
  int id;
  bool return_value = class_ids_.Lookup(function, &id);
  DCHECK(return_value);
  USE(return_value);
  return static_cast<uint32_t>(id);
}

uint32_t WebSnapshotSerializer::GetContextId(Context context) {
  int id;
  bool return_value = context_ids_.Lookup(context, &id);
  DCHECK(return_value);
  USE(return_value);
  return static_cast<uint32_t>(id);
}

uint32_t WebSnapshotSerializer::GetArrayId(JSArray array) {
  int id;
  bool return_value = array_ids_.Lookup(array, &id);
  DCHECK(return_value);
  USE(return_value);
  return static_cast<uint32_t>(id);
}

uint32_t WebSnapshotSerializer::GetObjectId(JSObject object) {
  int id;
  bool return_value = object_ids_.Lookup(object, &id);
  DCHECK(return_value);
  USE(return_value);
  return static_cast<uint32_t>(id);
}

WebSnapshotDeserializer::WebSnapshotDeserializer(v8::Isolate* isolate,
                                                 const uint8_t* data,
                                                 size_t buffer_size)
    : WebSnapshotDeserializer(reinterpret_cast<i::Isolate*>(isolate),
                              Handle<Object>(), {data, buffer_size}) {}

WebSnapshotDeserializer::WebSnapshotDeserializer(
    Isolate* isolate, Handle<Script> snapshot_as_script)
    : WebSnapshotDeserializer(
          isolate, handle(snapshot_as_script->name(), isolate),
          ExtractScriptBuffer(isolate, snapshot_as_script)) {}

WebSnapshotDeserializer::WebSnapshotDeserializer(
    Isolate* isolate, Handle<Object> script_name,
    base::Vector<const uint8_t> buffer)
    : WebSnapshotSerializerDeserializer(isolate),
      script_name_(script_name),
      deserializer_(isolate_, buffer.data(), buffer.length()) {
  isolate_->heap()->AddGCEpilogueCallback(UpdatePointersCallback,
                                          v8::kGCTypeAll, this);
  Handle<FixedArray> empty_array = isolate_->factory()->empty_fixed_array();
  strings_handle_ = empty_array;
  maps_handle_ = empty_array;
  contexts_handle_ = empty_array;
  functions_handle_ = empty_array;
  classes_handle_ = empty_array;
  arrays_handle_ = empty_array;
  objects_handle_ = empty_array;
}

WebSnapshotDeserializer::~WebSnapshotDeserializer() {
  isolate_->heap()->RemoveGCEpilogueCallback(UpdatePointersCallback, this);
}

void WebSnapshotDeserializer::UpdatePointers() {
  strings_ = *strings_handle_;
  maps_ = *maps_handle_;
  contexts_ = *contexts_handle_;
  functions_ = *functions_handle_;
  classes_ = *classes_handle_;
  arrays_ = *arrays_handle_;
  objects_ = *objects_handle_;
}

// static
base::Vector<const uint8_t> WebSnapshotDeserializer::ExtractScriptBuffer(
    Isolate* isolate, Handle<Script> snapshot_as_script) {
  Handle<String> source =
      handle(String::cast(snapshot_as_script->source()), isolate);
  if (source->IsExternalOneByteString()) {
    const v8::String::ExternalOneByteStringResource* resource =
        ExternalOneByteString::cast(*source).resource();
    return {reinterpret_cast<const uint8_t*>(resource->data()),
            resource->length()};
  } else if (source->IsSeqOneByteString()) {
    SeqOneByteString source_as_seq = SeqOneByteString::cast(*source);
    size_t length = source_as_seq.length();
    std::unique_ptr<uint8_t[]> data_copy(new uint8_t[length]);
    {
      DisallowGarbageCollection no_gc;
      uint8_t* data = source_as_seq.GetChars(no_gc);
      memcpy(data_copy.get(), data, length);
    }
    return {data_copy.get(), length};
  } else if (source->IsExternalTwoByteString()) {
    // TODO(v8:11525): Implement end-to-end snapshot processing which gets rid
    // of the need to copy the data here.
    const v8::String::ExternalStringResource* resource =
        ExternalTwoByteString::cast(*source).resource();
    size_t length = resource->length();
    std::unique_ptr<uint8_t[]> data_copy(new uint8_t[length]);
    {
      DisallowGarbageCollection no_gc;
      const uint16_t* data = resource->data();
      uint8_t* data_copy_ptr = data_copy.get();
      for (size_t i = 0; i < length; ++i) {
        data_copy_ptr[i] = static_cast<uint8_t>(data[i]);
      }
    }
    return {data_copy.get(), length};
  } else if (source->IsSeqTwoByteString()) {
    SeqTwoByteString source_as_seq = SeqTwoByteString::cast(*source);
    size_t length = source_as_seq.length();
    std::unique_ptr<uint8_t[]> data_copy(new uint8_t[length]);
    {
      DisallowGarbageCollection no_gc;
      uint16_t* data = source_as_seq.GetChars(no_gc);
      uint8_t* data_copy_ptr = data_copy.get();
      for (size_t i = 0; i < length; ++i) {
        data_copy_ptr[i] = static_cast<uint8_t>(data[i]);
      }
    }
    return {data_copy.get(), length};
  }
  UNREACHABLE();
}

void WebSnapshotDeserializer::Throw(const char* message) {
  string_count_ = 0;
  map_count_ = 0;
  context_count_ = 0;
  class_count_ = 0;
  function_count_ = 0;
  object_count_ = 0;
  deferred_references_->SetLength(0);

  // Make sure we don't read any more data
  deserializer_.position_ = deserializer_.end_;

  WebSnapshotSerializerDeserializer::Throw(message);
}

bool WebSnapshotDeserializer::Deserialize() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize);
  if (deserialized_) {
    Throw("Can't reuse WebSnapshotDeserializer");
    return false;
  }
  deserialized_ = true;
  auto buffer_size = deserializer_.end_ - deserializer_.position_;

  base::ElapsedTimer timer;
  if (FLAG_trace_web_snapshot) {
    timer.Start();
  }
  if (!DeserializeSnapshot()) {
    isolate_->ReportPendingMessages();
    return false;
  }
  if (!DeserializeScript()) {
    return false;
  }
  if (FLAG_trace_web_snapshot) {
    double ms = timer.Elapsed().InMillisecondsF();
    PrintF("[Deserializing snapshot (%zu bytes) took %0.3f ms]\n", buffer_size,
           ms);
  }
  return true;
}

bool WebSnapshotDeserializer::DeserializeSnapshot() {
  deferred_references_ = ArrayList::New(isolate_, 30);

  const void* magic_bytes;
  if (!deserializer_.ReadRawBytes(sizeof(kMagicNumber), &magic_bytes) ||
      memcmp(magic_bytes, kMagicNumber, sizeof(kMagicNumber)) != 0) {
    Throw("Invalid magic number");
    return false;
  }

  DeserializeStrings();
  DeserializeMaps();
  DeserializeContexts();
  DeserializeFunctions();
  DeserializeArrays();
  DeserializeObjects();
  DeserializeClasses();
  ProcessDeferredReferences();
  DeserializeExports();
  DCHECK_EQ(0, deferred_references_->Length());

  return !has_error();
}

bool WebSnapshotDeserializer::DeserializeScript() {
  // If there is more data, treat it as normal JavaScript.
  DCHECK_LE(deserializer_.position_, deserializer_.end_);
  auto remaining_bytes = deserializer_.end_ - deserializer_.position_;
  if (remaining_bytes > 0 && remaining_bytes < v8::String::kMaxLength) {
    v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate_);
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8(
            v8_isolate, reinterpret_cast<const char*>(deserializer_.position_),
            NewStringType::kNormal, static_cast<int>(remaining_bytes))
            .ToLocalChecked();

    ScriptOrigin origin(v8_isolate, Utils::ToLocal(script_name_));

    ScriptCompiler::Source script_source(source, origin);
    Local<UnboundScript> script;
    if (!ScriptCompiler::CompileUnboundScript(v8_isolate, &script_source)
             .ToLocal(&script)) {
      // The exception has already been reported.
      DCHECK(!isolate_->has_pending_exception());
      return false;
    }
    Local<Value> result;
    if (!script->BindToCurrentContext()
             ->Run(v8_isolate->GetCurrentContext())
             .ToLocal(&result)) {
      // The exception has already been reported.
      DCHECK(!isolate_->has_pending_exception());
      return false;
    }
  }

  // TODO(v8:11525): Add verification mode; verify the objects we just produced.
  return !has_error();
}

void WebSnapshotDeserializer::DeserializeStrings() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Strings);
  if (!deserializer_.ReadUint32(&string_count_) ||
      string_count_ > kMaxItemCount) {
    Throw("Malformed string table");
    return;
  }
  STATIC_ASSERT(kMaxItemCount <= FixedArray::kMaxLength);
  strings_handle_ = isolate_->factory()->NewFixedArray(string_count_);
  strings_ = *strings_handle_;
  for (uint32_t i = 0; i < string_count_; ++i) {
    MaybeHandle<String> maybe_string =
        deserializer_.ReadUtf8String(AllocationType::kOld);
    Handle<String> string;
    if (!maybe_string.ToHandle(&string)) {
      Throw("Malformed string");
      return;
    }
    strings_.set(i, *string);
  }
}

String WebSnapshotDeserializer::ReadString(bool internalize) {
  DCHECK(!strings_handle_->is_null());
  uint32_t string_id;
  if (!deserializer_.ReadUint32(&string_id) || string_id >= string_count_) {
    Throw("malformed string id\n");
    return ReadOnlyRoots(isolate_).empty_string();
  }
  String string = String::cast(strings_.get(string_id));
  if (internalize && !string.IsInternalizedString(isolate_)) {
    string = *isolate_->factory()->InternalizeString(handle(string, isolate_));
    strings_.set(string_id, string);
  }
  return string;
}

void WebSnapshotDeserializer::DeserializeMaps() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Maps);
  if (!deserializer_.ReadUint32(&map_count_) || map_count_ > kMaxItemCount) {
    Throw("Malformed shape table");
    return;
  }
  STATIC_ASSERT(kMaxItemCount <= FixedArray::kMaxLength);
  maps_handle_ = isolate_->factory()->NewFixedArray(map_count_);
  maps_ = *maps_handle_;
  for (uint32_t i = 0; i < map_count_; ++i) {
    uint32_t map_type;
    if (!deserializer_.ReadUint32(&map_type)) {
      Throw("Malformed shape");
      return;
    }
    bool has_custom_property_attributes;
    switch (map_type) {
      case PropertyAttributesType::DEFAULT:
        has_custom_property_attributes = false;
        break;
      case PropertyAttributesType::CUSTOM:
        has_custom_property_attributes = true;
        break;
      default:
        Throw("Unsupported map type");
        return;
    }

    uint32_t prototype_id;
    if (!deserializer_.ReadUint32(&prototype_id) ||
        prototype_id > kMaxItemCount) {
      Throw("Malformed shape");
      return;
    }

    uint32_t property_count;
    if (!deserializer_.ReadUint32(&property_count)) {
      Throw("Malformed shape");
      return;
    }
    // TODO(v8:11525): Consider passing the upper bound as a param and
    // systematically enforcing it on the ValueSerializer side.
    if (property_count > kMaxNumberOfDescriptors) {
      Throw("Malformed shape: too many properties");
      return;
    }

    if (property_count == 0) {
      DisallowGarbageCollection no_gc;
      Map empty_map =
          isolate_->native_context()->object_function().initial_map();
      maps_.set(i, empty_map);
      return;
    }

    Handle<DescriptorArray> descriptors =
        isolate_->factory()->NewDescriptorArray(property_count, 0);
    // for (uint32_t p = 0; p < property_count; ++p) {
    for (InternalIndex i : InternalIndex::Range(property_count)) {
      PropertyAttributes attributes = PropertyAttributes::NONE;
      if (has_custom_property_attributes) {
        uint32_t flags;
        if (!deserializer_.ReadUint32(&flags)) {
          Throw("Malformed shape");
          return;
        }
        attributes = FlagsToAttributes(flags);
      }

      Handle<String> key(ReadString(true), isolate_);

      // Use the "none" representation until we see the first object having this
      // map. At that point, modify the representation.
      Descriptor desc = Descriptor::DataField(
          isolate_, key, i.as_int(), attributes, Representation::None());
      descriptors->Set(i, &desc);
    }
    DCHECK_EQ(descriptors->number_of_descriptors(), property_count);
    descriptors->Sort();

    Handle<Map> map = isolate_->factory()->NewMap(
        JS_OBJECT_TYPE, JSObject::kHeaderSize * kTaggedSize, HOLEY_ELEMENTS, 0);
    map->InitializeDescriptors(isolate_, *descriptors);
    // TODO(v8:11525): Set 'constructor'.

    if (prototype_id == 0) {
      // Use Object.prototype as the prototype.
      map->set_prototype(isolate_->context().initial_object_prototype(),
                         UPDATE_WRITE_BARRIER);
    } else {
      // TODO(v8::11525): Implement stricter checks, e.g., disallow cycles.
      --prototype_id;
      if (prototype_id < current_object_count_) {
        map->set_prototype(HeapObject::cast(objects_.get(prototype_id)),
                           UPDATE_WRITE_BARRIER);
      } else {
        // The object hasn't been deserialized yet.
        AddDeferredReference(map, 0, OBJECT_ID, prototype_id);
      }
    }
    maps_.set(i, *map);
  }
}

void WebSnapshotDeserializer::DeserializeContexts() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Contexts);
  if (!deserializer_.ReadUint32(&context_count_) ||
      context_count_ > kMaxItemCount) {
    Throw("Malformed context table");
    return;
  }
  STATIC_ASSERT(kMaxItemCount <= FixedArray::kMaxLength);
  contexts_handle_ = isolate_->factory()->NewFixedArray(context_count_);
  contexts_ = *contexts_handle_;
  for (uint32_t i = 0; i < context_count_; ++i) {
    uint32_t context_type;
    if (!deserializer_.ReadUint32(&context_type)) {
      Throw("Malformed context type");
      return;
    }

    uint32_t parent_context_id;
    // Parent context is serialized before child context. Note: not >= on
    // purpose, we're going to subtract 1 later.
    if (!deserializer_.ReadUint32(&parent_context_id) ||
        parent_context_id > i) {
      Throw("Malformed context");
      return;
    }

    uint32_t variable_count;
    if (!deserializer_.ReadUint32(&variable_count)) {
      Throw("Malformed context");
      return;
    }
    // TODO(v8:11525): Enforce upper limit for variable count.
    Handle<ScopeInfo> scope_info =
        CreateScopeInfo(variable_count, parent_context_id > 0,
                        static_cast<ContextType>(context_type));

    Handle<Context> parent_context;
    if (parent_context_id > 0) {
      parent_context =
          handle(Context::cast(contexts_.get(parent_context_id - 1)), isolate_);
      scope_info->set_outer_scope_info(parent_context->scope_info());
    } else {
      parent_context = handle(isolate_->context(), isolate_);
    }

    const int context_local_base = ScopeInfo::kVariablePartIndex;
    const int context_local_info_base = context_local_base + variable_count;
    for (int variable_index = 0;
         variable_index < static_cast<int>(variable_count); ++variable_index) {
      {
        String name = ReadString(true);
        scope_info->set(context_local_base + variable_index, name);
      }

      // TODO(v8:11525): Support variable modes etc.
      uint32_t info =
          ScopeInfo::VariableModeBits::encode(VariableMode::kLet) |
          ScopeInfo::InitFlagBit::encode(
              InitializationFlag::kNeedsInitialization) |
          ScopeInfo::MaybeAssignedFlagBit::encode(
              MaybeAssignedFlag::kMaybeAssigned) |
          ScopeInfo::ParameterNumberBits::encode(
              ScopeInfo::ParameterNumberBits::kMax) |
          ScopeInfo::IsStaticFlagBit::encode(IsStaticFlag::kNotStatic);
      scope_info->set(context_local_info_base + variable_index,
                      Smi::FromInt(info));
    }

    // Allocate the FunctionContext after setting up the ScopeInfo to avoid
    // pointing to a ScopeInfo which is not set up yet.
    Handle<Context> context;
    switch (context_type) {
      case ContextType::FUNCTION:
        context =
            isolate_->factory()->NewFunctionContext(parent_context, scope_info);
        break;
      case ContextType::BLOCK:
        context =
            isolate_->factory()->NewBlockContext(parent_context, scope_info);
        break;
      default:
        Throw("Unsupported context type");
        return;
    }
    int context_header_length = scope_info->ContextHeaderLength();
    for (int variable_index = 0;
         variable_index < static_cast<int>(variable_count); ++variable_index) {
      int context_index = context_header_length + variable_index;
      Object value = ReadValue(context, context_index);
      context->set(context_index, value);
    }
    contexts_.set(i, *context);
  }
}

Handle<ScopeInfo> WebSnapshotDeserializer::CreateScopeInfo(
    uint32_t variable_count, bool has_parent, ContextType context_type) {
  // TODO(v8:11525): Decide how to handle language modes. (The code below sets
  // the language mode as strict.)
  // TODO(v8:11525): Support (context-allocating) receiver.
  // TODO(v8:11525): Support function variable & function name.
  // TODO(v8:11525): Support classes.

  ScopeType scope_type;
  int flags =
      ScopeInfo::SloppyEvalCanExtendVarsBit::encode(false) |
      ScopeInfo::LanguageModeBit::encode(LanguageMode::kStrict) |
      ScopeInfo::DeclarationScopeBit::encode(false) |
      ScopeInfo::ReceiverVariableBits::encode(VariableAllocationInfo::NONE) |
      ScopeInfo::HasClassBrandBit::encode(false) |
      ScopeInfo::HasSavedClassVariableBit::encode(false) |
      ScopeInfo::HasNewTargetBit::encode(false) |
      ScopeInfo::FunctionVariableBits::encode(VariableAllocationInfo::NONE) |
      ScopeInfo::HasInferredFunctionNameBit::encode(false) |
      ScopeInfo::IsAsmModuleBit::encode(false) |
      ScopeInfo::HasSimpleParametersBit::encode(false) |
      ScopeInfo::FunctionKindBits::encode(FunctionKind::kNormalFunction) |
      ScopeInfo::HasOuterScopeInfoBit::encode(has_parent) |
      ScopeInfo::IsDebugEvaluateScopeBit::encode(false) |
      ScopeInfo::ForceContextAllocationBit::encode(false) |
      ScopeInfo::PrivateNameLookupSkipsOuterClassBit::encode(false) |
      ScopeInfo::HasContextExtensionSlotBit::encode(false) |
      ScopeInfo::IsReplModeScopeBit::encode(false) |
      ScopeInfo::HasLocalsBlockListBit::encode(false);
  switch (context_type) {
    case ContextType::FUNCTION:
      scope_type = ScopeType::FUNCTION_SCOPE;
      flags |= ScopeInfo::DeclarationScopeBit::encode(true) |
               ScopeInfo::HasSimpleParametersBit::encode(true);
      break;
    case ContextType::BLOCK:
      scope_type = ScopeType::CLASS_SCOPE;
      flags |= ScopeInfo::ForceContextAllocationBit::encode(true);
      break;
    default:
      // Default to a CLASS_SCOPE, so that the rest of the code can be executed
      // without failures.
      scope_type = ScopeType::CLASS_SCOPE;
      Throw("Unsupported context type");
  }
  flags |= ScopeInfo::ScopeTypeBits::encode(scope_type);
  const int length = ScopeInfo::kVariablePartIndex +
                     (ScopeInfo::NeedsPositionInfo(scope_type)
                          ? ScopeInfo::kPositionInfoEntries
                          : 0) +
                     (has_parent ? 1 : 0) + 2 * variable_count;
  Handle<ScopeInfo> scope_info = isolate_->factory()->NewScopeInfo(length);
  {
    DisallowGarbageCollection no_gc;
    ScopeInfo raw = *scope_info;

    raw.set_flags(flags);
    DCHECK(!raw.IsEmpty());

    raw.set_context_local_count(variable_count);
    // TODO(v8:11525): Support parameters.
    raw.set_parameter_count(0);
    if (raw.HasPositionInfo()) {
      raw.SetPositionInfo(0, 0);
    }
  }
  return scope_info;
}

Handle<JSFunction> WebSnapshotDeserializer::CreateJSFunction(
    int shared_function_info_index, uint32_t start_position, uint32_t length,
    uint32_t parameter_count, uint32_t flags, uint32_t context_id) {
  // TODO(v8:11525): Deduplicate the SFIs for class methods.
  FunctionKind kind = FunctionFlagsToFunctionKind(flags);
  Handle<SharedFunctionInfo> shared =
      isolate_->factory()->NewSharedFunctionInfo(
          isolate_->factory()->empty_string(), MaybeHandle<Code>(),
          Builtin::kCompileLazy, kind);
  Handle<UncompiledData> uncompiled_data =
      isolate_->factory()->NewUncompiledDataWithoutPreparseData(
          ReadOnlyRoots(isolate_).empty_string_handle(), start_position,
          start_position + length);
  {
    DisallowGarbageCollection no_gc;
    SharedFunctionInfo raw = *shared;
    if (IsConciseMethod(kind)) {
      raw.set_syntax_kind(FunctionSyntaxKind::kAccessorOrMethod);
    }
    raw.set_script(*script_);
    raw.set_function_literal_id(shared_function_info_index);
    raw.set_internal_formal_parameter_count(JSParameterCount(parameter_count));
    // TODO(v8:11525): Decide how to handle language modes.
    raw.set_language_mode(LanguageMode::kStrict);
    raw.set_uncompiled_data(*uncompiled_data);
    raw.set_allows_lazy_compilation(true);
    shared_function_infos_.Set(shared_function_info_index,
                               HeapObjectReference::Weak(raw));
  }
  shared_function_info_table_ = ObjectHashTable::Put(
      shared_function_info_table_,
      handle(Smi::FromInt(start_position), isolate_),
      handle(Smi::FromInt(shared_function_info_index), isolate_));

  Handle<JSFunction> function =
      Factory::JSFunctionBuilder(isolate_, shared, isolate_->native_context())
          .Build();
  if (context_id > 0) {
    DCHECK_LT(context_id - 1, context_count_);
    // Guards raw pointer "context" below.
    DisallowHeapAllocation no_heap_access;
    Context context = Context::cast(contexts_.get(context_id - 1));
    function->set_context(context);
    shared->set_outer_scope_info(context.scope_info());
  }
  return function;
}

void WebSnapshotDeserializer::DeserializeFunctions() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Functions);
  if (!deserializer_.ReadUint32(&function_count_) ||
      function_count_ > kMaxItemCount) {
    Throw("Malformed function table");
    return;
  }
  STATIC_ASSERT(kMaxItemCount + 1 <= FixedArray::kMaxLength);
  functions_handle_ = isolate_->factory()->NewFixedArray(function_count_);
  functions_ = *functions_handle_;

  // Overallocate the array for SharedFunctionInfos; functions which we
  // deserialize soon will create more SharedFunctionInfos when called.
  shared_function_infos_handle_ = isolate_->factory()->NewWeakFixedArray(
      WeakArrayList::CapacityForLength(function_count_ + 1),
      AllocationType::kOld);
  shared_function_infos_ = *shared_function_infos_handle_;
  shared_function_info_table_ = ObjectHashTable::New(isolate_, function_count_);
  script_ = isolate_->factory()->NewScript(isolate_->factory()->empty_string());
  {
    DisallowGarbageCollection no_gc;
    Script raw = *script_;
    raw.set_type(Script::TYPE_WEB_SNAPSHOT);
    raw.set_shared_function_infos(shared_function_infos_);
    raw.set_shared_function_info_table(*shared_function_info_table_);
  }

  for (; current_function_count_ < function_count_; ++current_function_count_) {
    uint32_t context_id;
    // Note: > (not >= on purpose, we will subtract 1).
    if (!deserializer_.ReadUint32(&context_id) || context_id > context_count_) {
      Throw("Malformed function");
      return;
    }
    {
      String source = ReadString(false);
      DisallowGarbageCollection no_gc;
      if (current_function_count_ == 0) {
        script_->set_source(source);
      } else {
        // TODO(v8:11525): Support multiple source snippets.
        DCHECK_EQ(script_->source(), source);
      }
    }

    uint32_t start_position;
    uint32_t length;
    uint32_t parameter_count;
    uint32_t flags;
    if (!deserializer_.ReadUint32(&start_position) ||
        !deserializer_.ReadUint32(&length) ||
        !deserializer_.ReadUint32(&parameter_count) ||
        !deserializer_.ReadUint32(&flags)) {
      Throw("Malformed function");
      return;
    }

    // Index 0 is reserved for top-level shared function info (which web
    // snapshot scripts don't have).
    Handle<JSFunction> function =
        CreateJSFunction(current_function_count_ + 1, start_position, length,
                         parameter_count, flags, context_id);
    functions_.set(current_function_count_, *function);

    ReadFunctionPrototype(function);
  }
}

void WebSnapshotDeserializer::DeserializeClasses() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Classes);
  if (!deserializer_.ReadUint32(&class_count_) ||
      class_count_ > kMaxItemCount) {
    Throw("Malformed class table");
    return;
  }
  STATIC_ASSERT(kMaxItemCount + 1 <= FixedArray::kMaxLength);
  classes_handle_ = isolate_->factory()->NewFixedArray(class_count_);
  classes_ = *classes_handle_;

  // Grow the array for SharedFunctionInfos.
  shared_function_infos_handle_ = WeakFixedArray::EnsureSpace(
      isolate_, shared_function_infos_handle_,
      WeakArrayList::CapacityForLength(function_count_ + 1 + class_count_));
  shared_function_infos_ = *shared_function_infos_handle_;
  script_->set_shared_function_infos(shared_function_infos_);

  for (; current_class_count_ < class_count_; ++current_class_count_) {
    uint32_t context_id;
    // Note: > (not >= on purpose, we will subtract 1).
    if (!deserializer_.ReadUint32(&context_id) || context_id > context_count_) {
      Throw("Malformed class");
      return;
    }

    {
      String source = ReadString(false);
      if (current_function_count_ + current_class_count_ == 0) {
        script_->set_source(source);
      } else {
        // TODO(v8:11525): Support multiple source snippets.
        DCHECK_EQ(script_->source(), source);
      }
    }

    uint32_t start_position;
    uint32_t length;
    uint32_t parameter_count;
    uint32_t flags;
    if (!deserializer_.ReadUint32(&start_position) ||
        !deserializer_.ReadUint32(&length) ||
        !deserializer_.ReadUint32(&parameter_count) ||
        !deserializer_.ReadUint32(&flags)) {
      Throw("Malformed class");
      return;
    }

    // Index 0 is reserved for top-level shared function info (which web
    // snapshot scripts don't have).
    Handle<JSFunction> function = CreateJSFunction(
        function_count_ + current_class_count_ + 1, start_position, length,
        parameter_count, flags, context_id);
    classes_.set(current_class_count_, *function);

    ReadFunctionPrototype(function);
  }
}

void WebSnapshotDeserializer::DeserializeObjects() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Objects);
  if (!deserializer_.ReadUint32(&object_count_) ||
      object_count_ > kMaxItemCount) {
    Throw("Malformed objects table");
    return;
  }
  STATIC_ASSERT(kMaxItemCount <= FixedArray::kMaxLength);
  objects_handle_ = isolate_->factory()->NewFixedArray(object_count_);
  objects_ = *objects_handle_;
  for (; current_object_count_ < object_count_; ++current_object_count_) {
    uint32_t map_id;
    if (!deserializer_.ReadUint32(&map_id) || map_id >= map_count_) {
      Throw("Malformed object");
      return;
    }
    Map raw_map = Map::cast(maps_.get(map_id));
    Handle<DescriptorArray> descriptors =
        handle(raw_map.instance_descriptors(kRelaxedLoad), isolate_);
    int no_properties = raw_map.NumberOfOwnDescriptors();
    // TODO(v8:11525): In-object properties.
    Handle<Map> map(raw_map, isolate_);
    Handle<PropertyArray> property_array =
        isolate_->factory()->NewPropertyArray(no_properties);
    for (int i = 0; i < no_properties; ++i) {
      Object value = ReadValue(property_array, i);
      DisallowGarbageCollection no_gc;
      // Read the representation from the map.
      DescriptorArray raw_descriptors = *descriptors;
      PropertyDetails details = raw_descriptors.GetDetails(InternalIndex(i));
      CHECK_EQ(details.location(), PropertyLocation::kField);
      CHECK_EQ(PropertyKind::kData, details.kind());
      Representation r = details.representation();
      if (r.IsNone()) {
        // Switch over to wanted_representation.
        details = details.CopyWithRepresentation(Representation::Tagged());
        raw_descriptors.SetDetails(InternalIndex(i), details);
      } else if (!r.Equals(Representation::Tagged())) {
        // TODO(v8:11525): Support this case too.
        UNREACHABLE();
      }
      property_array->set(i, value);
    }
    Handle<JSObject> object = isolate_->factory()->NewJSObjectFromMap(map);
    object->set_raw_properties_or_hash(*property_array, kRelaxedStore);
    objects_.set(static_cast<int>(current_object_count_), *object);
  }
}

void WebSnapshotDeserializer::DeserializeArrays() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Arrays);
  if (!deserializer_.ReadUint32(&array_count_) ||
      object_count_ > kMaxItemCount) {
    Throw("Malformed array table");
    return;
  }
  STATIC_ASSERT(kMaxItemCount <= FixedArray::kMaxLength);
  arrays_handle_ = isolate_->factory()->NewFixedArray(array_count_);
  arrays_ = *arrays_handle_;
  for (; current_array_count_ < array_count_; ++current_array_count_) {
    uint32_t length;
    if (!deserializer_.ReadUint32(&length) || length > kMaxItemCount) {
      Throw("Malformed array");
      return;
    }
    Handle<FixedArray> elements = isolate_->factory()->NewFixedArray(length);
    ElementsKind elements_kind = PACKED_SMI_ELEMENTS;
    for (uint32_t i = 0; i < length; ++i) {
      Object value = ReadValue(elements, i);
      DisallowGarbageCollection no_gc;
      if (!value.IsSmi()) {
        elements_kind = PACKED_ELEMENTS;
      }
      elements->set(static_cast<int>(i), value);
    }
    Handle<JSArray> array = isolate_->factory()->NewJSArrayWithElements(
        elements, elements_kind, length);
    arrays_.set(static_cast<int>(current_array_count_), *array);
  }
}

void WebSnapshotDeserializer::DeserializeExports() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kWebSnapshotDeserialize_Exports);
  uint32_t count;
  if (!deserializer_.ReadUint32(&count) || count > kMaxItemCount) {
    Throw("Malformed export table");
    return;
  }
  // Pre-reserve the space for the properties we're going to add to the global
  // object.
  Handle<JSGlobalObject> global = isolate_->global_object();
  Handle<GlobalDictionary> dictionary(
      global->global_dictionary(isolate_, kAcquireLoad), isolate_);

  dictionary = GlobalDictionary::EnsureCapacity(
      isolate_, dictionary, dictionary->NumberOfElements() + count,
      AllocationType::kYoung);

  // TODO(v8:11525): The code below skips checks, in particular
  // LookupIterator::UpdateProtectors and
  // LookupIterator::ExtendingNonExtensible.
  InternalIndex entry = InternalIndex::NotFound();
  for (uint32_t i = 0; i < count; ++i) {
    Handle<String> export_name(ReadString(true), isolate_);
    // No deferred references should occur at this point, since all objects have
    // been deserialized.
    Object export_value = ReadValue();
    DisallowGarbageCollection no_gc;
    // Check for the correctness of the snapshot (thus far) before producing
    // something observable. TODO(v8:11525): Strictly speaking, we should
    // produce observable effects only when we know that the whole snapshot is
    // correct.
    if (has_error()) return;

    PropertyDetails property_details =
        PropertyDetails(PropertyKind::kData, NONE,
                        PropertyCell::InitialType(isolate_, export_value));
    Handle<Object> export_value_handle(export_value, isolate_);
    AllowGarbageCollection allow_gc;
    Handle<PropertyCell> transition_cell = isolate_->factory()->NewPropertyCell(
        export_name, property_details, export_value_handle);
    dictionary =
        GlobalDictionary::Add(isolate_, dictionary, export_name,
                              transition_cell, property_details, &entry);
  }

  global->set_global_dictionary(*dictionary, kReleaseStore);
  JSObject::InvalidatePrototypeChains(global->map(isolate_));
}

Object WebSnapshotDeserializer::ReadValue(
    Handle<HeapObject> object_for_deferred_reference,
    uint32_t index_for_deferred_reference) {
  uint32_t value_type;
  Factory* factory = isolate_->factory();
  // TODO(v8:11525): Consider adding a ReadByte.
  if (!deserializer_.ReadUint32(&value_type)) {
    Throw("Malformed variable");
    // Set "value" here so that the "keep on trucking" error handling won't fail
    // when dereferencing the handle.
    return Smi::zero();
  }
  switch (value_type) {
    case ValueType::FALSE_CONSTANT: {
      return ReadOnlyRoots(isolate_).false_value();
    }
    case ValueType::TRUE_CONSTANT: {
      return ReadOnlyRoots(isolate_).true_value();
    }
    case ValueType::NULL_CONSTANT: {
      return ReadOnlyRoots(isolate_).null_value();
    }
    case ValueType::UNDEFINED_CONSTANT: {
      return ReadOnlyRoots(isolate_).undefined_value();
    }
    case ValueType::INTEGER: {
      Maybe<int32_t> number = deserializer_.ReadZigZag<int32_t>();
      if (number.IsNothing()) {
        Throw("Malformed integer");
        return Smi::zero();
      }
      return *factory->NewNumberFromInt(number.FromJust());
    }
    case ValueType::DOUBLE: {
      double number;
      if (!deserializer_.ReadDouble(&number)) {
        Throw("Malformed double");
        return Smi::zero();
      }
      return *factory->NewNumber(number);
    }
    case ValueType::STRING_ID: {
      return ReadString(false);
    }
    case ValueType::ARRAY_ID:
      uint32_t array_id;
      if (!deserializer_.ReadUint32(&array_id) || array_id >= kMaxItemCount) {
        Throw("Malformed variable");
        return Smi::zero();
      }
      if (array_id < current_array_count_) {
        return arrays_.get(array_id);
      }
      // The array hasn't been deserialized yet.
      return AddDeferredReference(object_for_deferred_reference,
                                  index_for_deferred_reference, ARRAY_ID,
                                  array_id);
    case ValueType::OBJECT_ID:
      uint32_t object_id;
      if (!deserializer_.ReadUint32(&object_id) || object_id > kMaxItemCount) {
        Throw("Malformed variable");
        return Smi::zero();
      }
      if (object_id < current_object_count_) {
        return objects_.get(object_id);
      }
      // The object hasn't been deserialized yet.
      return AddDeferredReference(object_for_deferred_reference,
                                  index_for_deferred_reference, OBJECT_ID,
                                  object_id);
    case ValueType::FUNCTION_ID: {
      uint32_t function_id;
      if (!deserializer_.ReadUint32(&function_id) ||
          function_id >= function_count_) {
        Throw("Malformed object property");
        return Smi::zero();
      }
      if (function_id < current_function_count_) {
        return functions_.get(function_id);
      }
      // The function hasn't been deserialized yet.
      return AddDeferredReference(object_for_deferred_reference,
                                  index_for_deferred_reference, FUNCTION_ID,
                                  function_id);
    }
    case ValueType::CLASS_ID: {
      uint32_t class_id;
      if (!deserializer_.ReadUint32(&class_id) || class_id >= kMaxItemCount) {
        Throw("Malformed object property");
        return Smi::zero();
      }
      if (class_id < current_class_count_) {
        return classes_.get(class_id);
      }
      // The class hasn't been deserialized yet.
      return AddDeferredReference(object_for_deferred_reference,
                                  index_for_deferred_reference, CLASS_ID,
                                  class_id);
    }
    case ValueType::REGEXP: {
      Handle<String> pattern(ReadString(false), isolate_);
      Handle<String> flags_string(ReadString(false), isolate_);
      base::Optional<JSRegExp::Flags> flags =
          JSRegExp::FlagsFromString(isolate_, flags_string);
      if (!flags.has_value()) {
        Throw("Malformed flags in regular expression");
        return Smi::zero();
      }
      MaybeHandle<JSRegExp> maybe_regexp =
          JSRegExp::New(isolate_, pattern, flags.value());
      Handle<JSRegExp> regexp;
      if (!maybe_regexp.ToHandle(&regexp)) {
        Throw("Malformed RegExp");
        return Smi::zero();
      }
      return *regexp;
    }
    default:
      // TODO(v8:11525): Handle other value types.
      Throw("Unsupported value type");
      return Smi::zero();
  }
}

void WebSnapshotDeserializer::ReadFunctionPrototype(
    Handle<JSFunction> function) {
  uint32_t object_id;

  if (!deserializer_.ReadUint32(&object_id) || object_id > kMaxItemCount + 1) {
    Throw("Malformed class / function");
    return;
  }
  if (object_id == 0) {
    // No prototype.
    return;
  }
  --object_id;
  if (object_id < current_object_count_) {
    if (!SetFunctionPrototype(*function,
                              JSReceiver::cast(objects_.get(object_id)))) {
      Throw("Can't reuse function prototype");
      return;
    }
  } else {
    // The object hasn't been deserialized yet.
    AddDeferredReference(function, 0, OBJECT_ID, object_id);
  }
}

bool WebSnapshotDeserializer::SetFunctionPrototype(JSFunction function,
                                                   JSReceiver prototype) {
  DisallowGarbageCollection no_gc;
  // TODO(v8:11525): Enforce the invariant that no two prototypes share a map.
  Map map = prototype.map();
  map.set_is_prototype_map(true);
  if (!map.constructor_or_back_pointer().IsNullOrUndefined(isolate_)) {
    return false;
  }
  map.set_constructor_or_back_pointer(function);
  function.set_prototype_or_initial_map(prototype, kReleaseStore);
  return true;
}

HeapObject WebSnapshotDeserializer::AddDeferredReference(
    Handle<HeapObject> container, uint32_t index, ValueType target_type,
    uint32_t target_index) {
  if (container.is_null()) {
    const char* message = "Invalid reference";
    switch (target_type) {
      case ARRAY_ID:
        message = "Invalid array reference";
        break;
      case OBJECT_ID:
        message = "Invalid object reference";
        break;
      case CLASS_ID:
        message = "Invalid class reference";
        break;
      case FUNCTION_ID:
        message = "Invalid function reference";
        break;
      default:
        break;
    }
    Throw(message);
    return ReadOnlyRoots(isolate_).undefined_value();
  }
  DCHECK(container->IsPropertyArray() || container->IsContext() ||
         container->IsFixedArray() || container->IsJSFunction() ||
         container->IsMap());
  deferred_references_ = ArrayList::Add(
      isolate_, deferred_references_, container, Smi::FromInt(index),
      Smi::FromInt(target_type), Smi::FromInt(target_index));
  // Use HeapObject as placeholder since this might break elements kinds.
  return ReadOnlyRoots(isolate_).undefined_value();
}

void WebSnapshotDeserializer::ProcessDeferredReferences() {
  // Check for error now, since the FixedArrays below might not have been
  // created if there was an error.
  if (has_error()) {
    return;
  }

  DisallowGarbageCollection no_gc;
  ArrayList raw_deferred_references = *deferred_references_;

  // Deferred references is a list of (object, index, target type, target index)
  // tuples.
  for (int i = 0; i < raw_deferred_references.Length() - 3; i += 4) {
    HeapObject container = HeapObject::cast(raw_deferred_references.Get(i));
    int index = raw_deferred_references.Get(i + 1).ToSmi().value();
    ValueType target_type = static_cast<ValueType>(
        raw_deferred_references.Get(i + 2).ToSmi().value());
    int target_index = raw_deferred_references.Get(i + 3).ToSmi().value();
    Object target;
    switch (target_type) {
      case FUNCTION_ID:
        if (static_cast<uint32_t>(target_index) >= function_count_) {
          // Throw can allocate, but it's ok, since we're not using the raw
          // pointers after that.
          AllowGarbageCollection allow_gc;
          Throw("Invalid function reference");
          return;
        }
        target = functions_.get(target_index);
        break;
      case CLASS_ID:
        if (static_cast<uint32_t>(target_index) >= class_count_) {
          AllowGarbageCollection allow_gc;
          Throw("Invalid class reference");
          return;
        }
        target = classes_.get(target_index);
        break;
      case ARRAY_ID:
        if (static_cast<uint32_t>(target_index) >= array_count_) {
          AllowGarbageCollection allow_gc;
          Throw("Invalid array reference");
          return;
        }
        target = arrays_.get(target_index);
        break;
      case OBJECT_ID:
        if (static_cast<uint32_t>(target_index) >= object_count_) {
          AllowGarbageCollection allow_gc;
          Throw("Invalid object reference");
          return;
        }
        target = objects_.get(target_index);
        break;
      default:
        UNREACHABLE();
    }
    InstanceType instance_type = container.map().instance_type();
    if (InstanceTypeChecker::IsPropertyArray(instance_type)) {
      PropertyArray::cast(container).set(index, target);
    } else if (InstanceTypeChecker::IsContext(instance_type)) {
      Context::cast(container).set(index, target);
    } else if (InstanceTypeChecker::IsFixedArray(instance_type)) {
      FixedArray::cast(container).set(index, target);
    } else if (InstanceTypeChecker::IsJSFunction(instance_type)) {
      // The only deferred reference allowed for a JSFunction is the function
      // prototype.
      DCHECK_EQ(index, 0);
      DCHECK(target.IsJSReceiver());
      if (!SetFunctionPrototype(JSFunction::cast(container),
                                JSReceiver::cast(target))) {
        AllowGarbageCollection allow_gc;
        Throw("Can't reuse function prototype");
        return;
      }
    } else if (InstanceTypeChecker::IsMap(instance_type)) {
      // The only deferred reference allowed for a Map is the __proto__.
      DCHECK_EQ(index, 0);
      DCHECK(target.IsJSReceiver());
      Map::cast(container).set_prototype(HeapObject::cast(target),
                                         UPDATE_WRITE_BARRIER);
    } else {
      UNREACHABLE();
    }
  }
  deferred_references_->SetLength(0);
}

}  // namespace internal
}  // namespace v8
