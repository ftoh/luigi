// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#if defined(__i386__) || defined(_M_IX86)

#include "vendor/libcc/libcc.hh"
#include "ffi.hh"
#include "call.hh"
#include "util.hh"

#include <napi.h>

namespace RG {

extern "C" uint64_t ForwardCallG(const void *func, uint8_t *sp);
extern "C" float ForwardCallF(const void *func, uint8_t *sp);
extern "C" double ForwardCallD(const void *func, uint8_t *sp);
extern "C" uint64_t ForwardCallRG(const void *func, uint8_t *sp);
extern "C" float ForwardCallRF(const void *func, uint8_t *sp);
extern "C" double ForwardCallRD(const void *func, uint8_t *sp);

static inline bool IsRegular(Size size)
{
    bool regular = (size <= 8 && !(size & (size - 1)));
    return regular;
}

bool AnalyseFunction(InstanceData *instance, FunctionInfo *func)
{
    int fast = (func->convention == CallConvention::Fastcall) ? 2 : 0;

    if (func->ret.type->primitive != PrimitiveKind::Record) {
        func->ret.trivial = true;
#if defined(_WIN32) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    } else {
        func->ret.trivial = IsRegular(func->ret.type->size);
#endif
    }
#ifndef _WIN32
    if (fast && !func->ret.trivial) {
        func->ret.fast = true;
        fast--;
    }
#endif

    Size params_size = 0;
    for (ParameterInfo &param: func->parameters) {
        if (fast && param.type->size <= 4) {
            param.fast = true;
            fast--;
        }

        params_size += std::max((int16_t)4, param.type->size);
    }
    func->args_size = params_size + 4 * !func->ret.trivial;

    switch (func->convention) {
        case CallConvention::Cdecl: {
            func->decorated_name = Fmt(&instance->str_alloc, "_%1", func->name).ptr;
        } break;
        case CallConvention::Stdcall: {
            RG_ASSERT(!func->variadic);
            func->decorated_name = Fmt(&instance->str_alloc, "_%1@%2", func->name, params_size).ptr;
        } break;
        case CallConvention::Fastcall: {
            RG_ASSERT(!func->variadic);
            func->decorated_name = Fmt(&instance->str_alloc, "@%1@%2", func->name, params_size).ptr;
            func->args_size += 16;
        } break;
    }

    return true;
}

Napi::Value CallData::Execute(const Napi::CallbackInfo &info)
{
    // Sanity checks
    if (info.Length() < (uint32_t)func->parameters.len) {
        ThrowError<Napi::TypeError>(env, "Expected %1 arguments, got %2", func->parameters.len, info.Length());
        return env.Null();
    }

    uint8_t *return_ptr = nullptr;
    uint32_t *args_ptr = nullptr;
    uint32_t *fast_ptr = nullptr;

    // Pass return value in register or through memory
    if (RG_UNLIKELY(!AllocStack(func->args_size, 16, &args_ptr)))
        return env.Null();
    if (func->convention == CallConvention::Fastcall) {
        fast_ptr = args_ptr;
        args_ptr += 4;
    }
    if (!func->ret.trivial) {
        if (RG_UNLIKELY(!AllocHeap(func->ret.type->size, 16, &return_ptr)))
            return env.Null();
        *((func->ret.fast ? fast_ptr : args_ptr)++) = (uint32_t)return_ptr;
    }

    LocalArray<OutObject, MaxOutParameters> out_objects;

    // Push arguments
    for (Size i = 0; i < func->parameters.len; i++) {
        const ParameterInfo &param = func->parameters[i];
        RG_ASSERT(param.directions >= 1 && param.directions <= 3);

        Napi::Value value = info[param.offset];

        switch (param.type->primitive) {
            case PrimitiveKind::Void: { RG_UNREACHABLE(); } break;

            case PrimitiveKind::Bool: {
                if (RG_UNLIKELY(!value.IsBoolean())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected boolean", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                bool b = value.As<Napi::Boolean>();
                *(bool *)((param.fast ? fast_ptr : args_ptr)++) = b;
            } break;
            case PrimitiveKind::Int8:
            case PrimitiveKind::UInt8:
            case PrimitiveKind::Int16:
            case PrimitiveKind::UInt16:
            case PrimitiveKind::Int32:
            case PrimitiveKind::UInt32: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                int32_t v = CopyNumber<int32_t>(value);
                *((param.fast ? fast_ptr : args_ptr)++) = (uint32_t)v;
            } break;
            case PrimitiveKind::Int64:
            case PrimitiveKind::UInt64: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                int64_t v = CopyNumber<int64_t>(value);
                *(uint64_t *)args_ptr = (uint64_t)v;
                args_ptr += 2;
            } break;
            case PrimitiveKind::Float32: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                float f = CopyNumber<float>(value);
                *(float *)((param.fast ? fast_ptr : args_ptr)++) = f;
            } break;
            case PrimitiveKind::Float64: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                double d = CopyNumber<double>(value);
                *(double *)args_ptr = d;
                args_ptr += 2;
            } break;
            case PrimitiveKind::String: {
                const char *str;
                if (RG_LIKELY(value.IsString())) {
                    str = PushString(value);
                    if (RG_UNLIKELY(!str))
                        return env.Null();
                } else if (IsNullOrUndefined(value)) {
                    str = nullptr;
                } else {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected string", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                *(const char **)((param.fast ? fast_ptr : args_ptr)++) = str;
            } break;
            case PrimitiveKind::String16: {
                const char16_t *str16;
                if (RG_LIKELY(value.IsString())) {
                    str16 = PushString16(value);
                    if (RG_UNLIKELY(!str16))
                        return env.Null();
                } else if (IsNullOrUndefined(value)) {
                    str16 = nullptr;
                } else {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected string", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                *(const char16_t **)((param.fast ? fast_ptr : args_ptr)++) = str16;
            } break;
            case PrimitiveKind::Pointer: {
                uint8_t *ptr;

                if (CheckValueTag(instance, value, param.type)) {
                    ptr = value.As<Napi::External<uint8_t>>().Data();
                } else if (IsObject(value) && param.type->ref->primitive == PrimitiveKind::Record) {
                    Napi::Object obj = value.As<Napi::Object>();

                    if (RG_UNLIKELY(!AllocHeap(param.type->ref->size, 16, &ptr)))
                        return env.Null();

                    if (param.directions & 1) {
                        if (!PushObject(obj, param.type->ref, ptr))
                            return env.Null();
                    } else {
                        memset(ptr, 0, param.type->size);
                    }
                    if (param.directions & 2) {
                        OutObject out = {obj, ptr, param.type->ref};
                        out_objects.Append(out);
                    }
                } else if (IsNullOrUndefined(value)) {
                    ptr = nullptr;
                } else {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected %3", GetValueType(instance, value), i + 1, param.type->name);
                    return env.Null();
                }

                *(uint8_t **)((param.fast ? fast_ptr : args_ptr)++) = ptr;
            } break;

            case PrimitiveKind::Record: {
                if (RG_UNLIKELY(!IsObject(value))) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected object", GetValueType(instance, value), i + 1);
                    return env.Null();
                }

                Napi::Object obj = value.As<Napi::Object>();

                if (param.fast) {
                    uint8_t *ptr = (uint8_t *)(fast_ptr++);
                    if (!PushObject(obj, param.type, ptr))
                        return env.Null();
                } else {
                    uint8_t *ptr = (uint8_t *)AlignUp(args_ptr, param.type->align);
                    if (!PushObject(obj, param.type, ptr))
                        return env.Null();
                    args_ptr = (uint32_t *)AlignUp(ptr + param.type->size, 4);
                }
            } break;
        }
    }

    if (instance->debug) {
        DumpDebug();
    }

#define PERFORM_CALL(Suffix) \
        ([&]() { \
            auto ret = (func->convention == CallConvention::Fastcall ? ForwardCallR ## Suffix(func->func, GetSP()) \
                                                                     : ForwardCall ## Suffix(func->func, GetSP())); \
            PopOutArguments(out_objects); \
            return ret; \
        })()

    // Execute and convert return value
    switch (func->ret.type->primitive) {
        case PrimitiveKind::Void: {
            PERFORM_CALL(G);
            return env.Null();
        } break;
        case PrimitiveKind::Bool: {
            uint32_t rax = (uint32_t)PERFORM_CALL(G);
            return Napi::Boolean::New(env, rax);
        } break;
        case PrimitiveKind::Int8:
        case PrimitiveKind::UInt8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::UInt16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::UInt32: {
            uint32_t rax = (uint32_t)PERFORM_CALL(G);
            return Napi::Number::New(env, (double)rax);
        } break;
        case PrimitiveKind::Int64: {
            uint64_t ret = PERFORM_CALL(G);
            return Napi::BigInt::New(env, (int64_t)ret);
        } break;
        case PrimitiveKind::UInt64: {
            uint64_t ret = PERFORM_CALL(G);
            return Napi::BigInt::New(env, ret);
        } break;
        case PrimitiveKind::Float32: {
            float f = PERFORM_CALL(F);
            return Napi::Number::New(env, (double)f);
        } break;
        case PrimitiveKind::Float64: {
            double d = PERFORM_CALL(D);
            return Napi::Number::New(env, d);
        } break;
        case PrimitiveKind::String: {
            uint32_t rax = (uint32_t)PERFORM_CALL(G);
            return Napi::String::New(env, (const char *)rax);
        } break;
        case PrimitiveKind::String16: {
            uint32_t rax = (uint32_t)PERFORM_CALL(G);
            return Napi::String::New(env, (const char16_t *)rax);
        } break;
        case PrimitiveKind::Pointer: {
            uint32_t rax = (uint32_t)PERFORM_CALL(G);
            void *ptr = (void *)rax;

            Napi::External<void> external = Napi::External<void>::New(env, ptr);
            SetValueTag(instance, external, func->ret.type);

            return external;
        } break;

        case PrimitiveKind::Record: {
            uint64_t ret = PERFORM_CALL(G);
            const uint8_t *ptr = return_ptr ? return_ptr : (const uint8_t *)&ret;

            Napi::Object obj = PopObject(env, ptr, func->ret.type);
            return obj;
        } break;
    }

#undef PERFORM_CALL

    RG_UNREACHABLE();
}

}

#endif
