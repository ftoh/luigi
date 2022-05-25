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

#ifdef __riscv

#include "vendor/libcc/libcc.hh"
#include "ffi.hh"
#include "call.hh"
#include "util.hh"

#include <napi.h>

namespace RG {

struct A0A1Ret {
    uint64_t a0;
    uint64_t a1;
};
struct A0Fa0Ret {
    uint64_t a0;
    double fa0;
};
struct Fa0A0Ret {
    double fa0;
    uint64_t a0;
};
struct Fa0Fa1Ret {
    double fa0;
    double fa1;
};

extern "C" A0A1Ret ForwardCallGG(const void *func, uint8_t *sp);
extern "C" float ForwardCallF(const void *func, uint8_t *sp);
extern "C" Fa0A0Ret ForwardCallDG(const void *func, uint8_t *sp);
extern "C" A0Fa0Ret ForwardCallGD(const void *func, uint8_t *sp);
extern "C" Fa0Fa1Ret ForwardCallDD(const void *func, uint8_t *sp);

extern "C" A0A1Ret ForwardCallXGG(const void *func, uint8_t *sp);
extern "C" float ForwardCallXF(const void *func, uint8_t *sp);
extern "C" Fa0A0Ret ForwardCallXDG(const void *func, uint8_t *sp);
extern "C" A0Fa0Ret ForwardCallXGD(const void *func, uint8_t *sp);
extern "C" Fa0Fa1Ret ForwardCallXDD(const void *func, uint8_t *sp);

static void AnalyseParameter(ParameterInfo *param, int gpr_avail, int vec_avail)
{
    gpr_avail = std::min(2, gpr_avail);
    vec_avail = std::min(2, vec_avail);

    if (param->type->size > 16) {
        param->gpr_count = gpr_avail ? 1 : 0;
        param->use_memory = true;

        return;
    }

    int gpr_count = 0;
    int vec_count = 0;
    bool gpr_first = false;

    AnalyseFlat(param->type, [&](const TypeInfo *type, int offset, int count) {
        if (IsFloat(type)) {
            vec_count += count;
        } else {
            gpr_count += count;
            gpr_first |= !vec_count;
        }
    });

    if (gpr_count == 1 && vec_count == 1 && gpr_avail && vec_avail) {
        param->gpr_count = 1;
        param->vec_count = 1;
        param->gpr_first = gpr_first;
    } else if (vec_count && !gpr_count && vec_count <= vec_avail) {
        param->vec_count = vec_count;
    } else if (gpr_avail) {
        param->gpr_count = (param->type->size + 7) / 8;
        param->gpr_first = true;
    }
}

bool AnalyseFunction(InstanceData *, FunctionInfo *func)
{
    const int treshold = (__riscv_xlen / 4); // 8 for RV32, 16 for RV64

    AnalyseParameter(&func->ret, 2, 2);

    int gpr_avail = 8 - func->ret.use_memory;
    int vec_avail = 8;

    for (ParameterInfo &param: func->parameters) {
        AnalyseParameter(&param, gpr_avail, !param.variadic ? vec_avail : 0);

        gpr_avail = std::max(0, gpr_avail - param.gpr_count);
        vec_avail = std::max(0, vec_avail - param.vec_count);
    }

    func->args_size = treshold * func->parameters.len;
    func->forward_fp = (vec_avail < 8);

    return true;
}

bool CallData::Prepare(const Napi::CallbackInfo &info)
{
    uint8_t *args_ptr = nullptr;
#if __riscv_xlen == 64
    uint64_t *gpr_ptr = nullptr;
    uint64_t *vec_ptr = nullptr;
#else
    uint32_t *gpr_ptr = nullptr;
    uint32_t *vec_ptr = nullptr;
#endif

    // Return through registers unless it's too big
    if (RG_UNLIKELY(!AllocStack(func->args_size, 16, &args_ptr)))
        return false;
    if (RG_UNLIKELY(!AllocStack(8 * RG_SIZE(void *), 8, &gpr_ptr)))
        return false;
    if (RG_UNLIKELY(!AllocStack(8 * 8, 8, &vec_ptr)))
        return false;
    if (func->ret.use_memory) {
        if (RG_UNLIKELY(!AllocHeap(func->ret.type->size, 16, &return_ptr)))
            return false;
        *(uint8_t **)(gpr_ptr++) = return_ptr;
    }

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
                    return false;
                }

                bool b = value.As<Napi::Boolean>();

                if (RG_LIKELY(param.gpr_count)) {
                    *(gpr_ptr++) = (uint64_t)b;
                } else {
                    *(uint64_t *)args_ptr = (uint64_t)b;
                    args_ptr += 8;
                }
            } break;
            case PrimitiveKind::Int8:
            case PrimitiveKind::Int16:
            case PrimitiveKind::Int32:
            case PrimitiveKind::Int64: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return false;
                }

                int64_t v = CopyNumber<int64_t>(value);

                if (RG_LIKELY(param.gpr_count)) {
                    *(int64_t *)(gpr_ptr++) = v;
                } else {
                    *(int64_t *)args_ptr = v;
                    args_ptr += 8;
                }
            } break;
            case PrimitiveKind::UInt8:
            case PrimitiveKind::UInt16:
            case PrimitiveKind::UInt32:
            case PrimitiveKind::UInt64: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return false;
                }

                uint64_t v = CopyNumber<uint64_t>(value);

                if (RG_LIKELY(param.gpr_count)) {
                    *(uint64_t *)(gpr_ptr++) = v;
                } else {
                    *(uint64_t *)args_ptr = v;
                    args_ptr += 8;
                }
            } break;
            case PrimitiveKind::String: {
                const char *str;
                if (RG_LIKELY(value.IsString())) {
                    str = PushString(value);
                    if (RG_UNLIKELY(!str))
                        return false;
                } else if (IsNullOrUndefined(value)) {
                    str = nullptr;
                } else {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected string", GetValueType(instance, value), i + 1);
                    return false;
                }

                if (RG_LIKELY(param.gpr_count)) {
                    *(const char **)(gpr_ptr++) = str;
                } else {
                    *(const char **)args_ptr = str;
                    args_ptr += 8;
                }
            } break;
            case PrimitiveKind::String16: {
                const char16_t *str16;
                if (RG_LIKELY(value.IsString())) {
                    str16 = PushString16(value);
                    if (RG_UNLIKELY(!str16))
                        return false;
                } else if (IsNullOrUndefined(value)) {
                    str16 = nullptr;
                } else {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected string", GetValueType(instance, value), i + 1);
                    return false;
                }

                if (RG_LIKELY(param.gpr_count)) {
                    *(const char16_t **)(gpr_ptr++) = str16;
                } else {
                    *(const char16_t **)args_ptr = str16;
                    args_ptr += 8;
                }
            } break;
            case PrimitiveKind::Pointer: {
                uint8_t *ptr;

                if (CheckValueTag(instance, value, param.type)) {
                    ptr = value.As<Napi::External<uint8_t>>().Data();
                } else if (IsObject(value) && param.type->ref->primitive == PrimitiveKind::Record) {
                    Napi::Object obj = value.As<Napi::Object>();

                    if (RG_UNLIKELY(!AllocHeap(param.type->ref->size, 16, &ptr)))
                        return false;

                    if (param.directions & 1) {
                        if (!PushObject(obj, param.type->ref, ptr))
                            return false;
                    } else {
                        memset(ptr, 0, param.type->size);
                    }
                    if (param.directions & 2) {
                        OutObject *out = out_objects.AppendDefault();

                        out->ref.Reset(obj, 1);
                        out->ptr = ptr;
                        out->type = param.type->ref;
                    }
                } else if (IsNullOrUndefined(value)) {
                    ptr = nullptr;
                } else {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected %3", GetValueType(instance, value), i + 1, param.type->name);
                    return false;
                }

                if (RG_LIKELY(param.gpr_count)) {
                    *(uint8_t **)(gpr_ptr++) = ptr;
                } else {
                    *(uint8_t **)args_ptr = ptr;
                    args_ptr += 8;
                }
            } break;
            case PrimitiveKind::Record: {
                if (RG_UNLIKELY(!IsObject(value))) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected object", GetValueType(instance, value), i + 1);
                    return false;
                }

                Napi::Object obj = value.As<Napi::Object>();

                if (!param.use_memory) {
                    RG_ASSERT(param.type->size <= 16);

                    // Split float or mixed int-float structs to registers
                    int realign = param.vec_count ? 8 : 0;

                    uint64_t buf[2] = { 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull };
                    if (!PushObject(obj, param.type, (uint8_t *)buf, realign))
                        return false;
                    uint64_t *ptr = buf;

                    if (param.gpr_first) {
                        *(gpr_ptr++) = *(ptr++);
                        *((param.vec_count ? vec_ptr : gpr_ptr)++) = *(ptr++);
                        gpr_ptr -= (param.gpr_count == 1);
                    } else if (param.vec_count) {
                        *(vec_ptr++) = *(ptr++);
                        *((param.gpr_count ? gpr_ptr : vec_ptr)++) = *(ptr++);
                    } else {
                        RG_ASSERT(param.type->align <= 8);

                        memcpy_safe(args_ptr, ptr, param.type->size);
                        args_ptr += AlignLen(param.type->size, 8);
                    }
                } else {
                    uint8_t *ptr;
                    if (RG_UNLIKELY(!AllocHeap(param.type->size, 16, &ptr)))
                        return false;

                    if (param.gpr_count) {
                        RG_ASSERT(param.gpr_count == 1);
                        RG_ASSERT(param.vec_count == 0);

                        *(uint8_t **)(gpr_ptr++) = ptr;
                    } else {
                        *(uint8_t **)args_ptr = ptr;
                        args_ptr += 8;
                    }

                    if (!PushObject(obj, param.type, ptr))
                        return false;
                }
            } break;
            case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
            case PrimitiveKind::Float32: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return false;
                }

                float f = CopyNumber<float>(value);

                if (RG_LIKELY(param.vec_count)) {
                    memset((uint8_t *)vec_ptr + 4, 0xFF, 4);
                    *(float *)(vec_ptr++) = f;
                } else if (param.gpr_count) {
                    memset((uint8_t *)gpr_ptr + 4, 0xFF, 4);
                    *(float *)(gpr_ptr++) = f;
                } else {
                    memset(args_ptr + 4, 0xFF, 4);
                    *(float *)args_ptr = f;
                }
            } break;
            case PrimitiveKind::Float64: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value for argument %2, expected number", GetValueType(instance, value), i + 1);
                    return false;
                }

                double d = CopyNumber<double>(value);

                if (RG_LIKELY(param.vec_count)) {
                    *(double *)(vec_ptr++) = d;
                } else if (param.gpr_count) {
                    *(double *)(gpr_ptr++) = d;
                } else {
                    *(double *)args_ptr = d;
                    args_ptr += 8;
                }
            } break;
        }
    }

    sp = mem->stack.end();

    return true;
}

void CallData::Execute()
{
#define PERFORM_CALL(Suffix) \
        ([&]() { \
            auto ret = (func->forward_fp ? ForwardCallX ## Suffix(func->func, sp) \
                                         : ForwardCall ## Suffix(func->func, sp)); \
            return ret; \
        })()

    // Execute and convert return value
    switch (func->ret.type->primitive) {
        case PrimitiveKind::Void:
        case PrimitiveKind::Bool:
        case PrimitiveKind::Int8:
        case PrimitiveKind::UInt8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::UInt16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::UInt32:
        case PrimitiveKind::Int64:
        case PrimitiveKind::UInt64:
        case PrimitiveKind::String:
        case PrimitiveKind::String16:
        case PrimitiveKind::Pointer: { result.u64 = PERFORM_CALL(GG).a0; } break;
        case PrimitiveKind::Record: {
            if (func->ret.gpr_first && !func->ret.vec_count) {
                A0A1Ret ret = PERFORM_CALL(GG);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            } else if (func->ret.gpr_first) {
                A0Fa0Ret ret = PERFORM_CALL(GD);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            } else if (func->ret.vec_count == 2) {
                Fa0Fa1Ret ret = PERFORM_CALL(DD);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            } else {
                Fa0A0Ret ret = PERFORM_CALL(DG);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            }
        } break;
        case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
        case PrimitiveKind::Float32: { result.f = PERFORM_CALL(F); } break;
        case PrimitiveKind::Float64: { result.d = PERFORM_CALL(DD).fa0; } break;
    }

#undef PERFORM_CALL
}

Napi::Value CallData::Complete()
{
    for (const OutObject &out: out_objects) {
        Napi::Object obj = out.ref.Value().As<Napi::Object>();
        PopObject(obj, out.ptr, out.type);
    }

    switch (func->ret.type->primitive) {
        case PrimitiveKind::Void: return env.Null();
        case PrimitiveKind::Bool: return Napi::Boolean::New(env, result.u32);
        case PrimitiveKind::Int8:
        case PrimitiveKind::UInt8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::UInt16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::UInt32: return Napi::Number::New(env, (double)result.u32);
        case PrimitiveKind::Int64: return Napi::BigInt::New(env, (int64_t)result.u64);
        case PrimitiveKind::UInt64: return Napi::BigInt::New(env, result.u64);
        case PrimitiveKind::String: return Napi::String::New(env, (const char *)result.ptr);
        case PrimitiveKind::String16: return Napi::String::New(env, (const char16_t *)result.ptr);
        case PrimitiveKind::Pointer: {
            Napi::External<void> external = Napi::External<void>::New(env, result.ptr);
            SetValueTag(instance, external, func->ret.type);

            return external;
        } break;
        case PrimitiveKind::Record: {
            if (func->ret.vec_count) { // HFA
                Napi::Object obj = PopObject((const uint8_t *)&result.buf, func->ret.type, 8);
                return obj;
            } else {
                const uint8_t *ptr = return_ptr ? (const uint8_t *)return_ptr
                                                : (const uint8_t *)&result.buf;

                Napi::Object obj = PopObject(ptr, func->ret.type);
                return obj;
            }
        } break;
        case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
        case PrimitiveKind::Float32: return Napi::Number::New(env, (double)result.f);
        case PrimitiveKind::Float64: return Napi::Number::New(env, result.d);
    }

    RG_UNREACHABLE();
}

}

#endif