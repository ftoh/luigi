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

#if defined(__aarch64__)

#include "libcc.hh"
#include "ffi.hh"
#include "call.hh"

#include <napi.h>

namespace RG {

enum class RegisterClass {
    NoClass = 0, // Explicitly 0
    Integer,
    SSE,
    Memory
};

struct X0X1Ret {
    uint64_t x0;
    uint64_t x1;
};
struct X0D0Ret {
    uint64_t x0;
    double d0;
};
struct D0X0Ret {
    double d0;
    uint64_t x0;
};
struct D0D1Ret {
    double d0;
    double d1;
};

extern "C" X0X1Ret ForwardCallGG(const void *func, uint8_t *sp);
extern "C" float ForwardCallF(const void *func, uint8_t *sp);
extern "C" D0X0Ret ForwardCallDG(const void *func, uint8_t *sp);
extern "C" X0D0Ret ForwardCallGD(const void *func, uint8_t *sp);
extern "C" D0D1Ret ForwardCallDD(const void *func, uint8_t *sp);

extern "C" X0X1Ret ForwardCallXGG(const void *func, uint8_t *sp);
extern "C" float ForwardCallXF(const void *func, uint8_t *sp);
extern "C" D0X0Ret ForwardCallXDG(const void *func, uint8_t *sp);
extern "C" X0D0Ret ForwardCallXGD(const void *func, uint8_t *sp);
extern "C" D0D1Ret ForwardCallXDD(const void *func, uint8_t *sp);

static inline RegisterClass MergeClasses(RegisterClass cls1, RegisterClass cls2)
{
    if (cls1 == cls2)
        return cls1;

    if (cls1 == RegisterClass::NoClass)
        return cls2;
    if (cls2 == RegisterClass::NoClass)
        return cls1;

    if (cls1 == RegisterClass::Memory || cls2 == RegisterClass::Memory)
        return RegisterClass::Memory;
    if (cls1 == RegisterClass::Integer || cls2 == RegisterClass::Integer)
        return RegisterClass::Integer;

    return RegisterClass::SSE;
}

static Size ClassifyType(const TypeInfo *type, Size offset, Span<RegisterClass> classes)
{
    RG_ASSERT(classes.len > 0);

    switch (type->primitive) {
        case PrimitiveKind::Void: { return 0; } break;

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
        case PrimitiveKind::Pointer: {
            classes[0] = MergeClasses(classes[0], RegisterClass::Integer);
            return 1;
        } break;

        case PrimitiveKind::Float32:
        case PrimitiveKind::Float64: {
            classes[0] = MergeClasses(classes[0], RegisterClass::SSE);
            return 1;
        } break;

        case PrimitiveKind::Record: {
            if (type->size > 64) {
                classes[0] = MergeClasses(classes[0], RegisterClass::Memory);
                return 1;
            }

            for (const RecordMember &member: type->members) {
                Size start = offset / 8;
                ClassifyType(member.type, offset % 8, classes.Take(start, classes.len - start));
                offset += member.type->size;
            }

            return (offset + 7) / 8;
        } break;
    }

    RG_UNREACHABLE();
}

static void AnalyseParameter(ParameterInfo *param, int gpr_avail, int vec_avail)
{
    LocalArray<RegisterClass, 8> classes = {};
    classes.len = ClassifyType(param->type, 0, classes.data);

    if (!classes.len)
        return;
    if (classes.len > 2)
        return;

    int gpr_count = 0;
    int vec_count = 0;

    for (RegisterClass cls: classes) {
        RG_ASSERT(cls != RegisterClass::NoClass);

        if (cls == RegisterClass::Memory)
            return;

        gpr_count += (cls == RegisterClass::Integer);
        vec_count += (cls == RegisterClass::SSE);
    }

    if (gpr_count <= gpr_avail && vec_count <= vec_avail){
        param->gpr_count = (int8_t)gpr_count;
        param->vec_count = (int8_t)vec_count;
        param->gpr_first = (classes[0] == RegisterClass::Integer);
    }
}

bool AnalyseFunction(FunctionInfo *func)
{
    AnalyseParameter(&func->ret, 2, 2);

    int gpr_avail = 8;
    int vec_avail = 8;

    for (ParameterInfo &param: func->parameters) {
        AnalyseParameter(&param, gpr_avail, vec_avail);

        gpr_avail -= param.gpr_count;
        vec_avail -= param.vec_count;
    }

    return true;
}

Napi::Value TranslateCall(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    FunctionInfo *func = (FunctionInfo *)info.Data();
    LibraryData *lib = func->lib.get();

    RG_DEFER { lib->tmp_alloc.ReleaseAll(); };

    // Sanity checks
    if (info.Length() < (uint32_t)func->parameters.len) {
        ThrowError<Napi::TypeError>(env, "Expected %1 arguments, got %2", func->parameters.len, info.Length());
        return env.Null();
    }

    // Stack pointer and register
    uint8_t *top_ptr = lib->stack.end();
    uint8_t *return_ptr = nullptr;
    uint8_t *args_ptr = nullptr;
    uint64_t *gpr_ptr = nullptr, *vec_ptr = nullptr;
    int gpr_count = 0, vec_count = 0;

    // Return through registers unless it's too big
    if (!func->ret.type->size || func->ret.gpr_count || func->ret.vec_count) {
        args_ptr = top_ptr - func->args_size;
        vec_ptr = (uint64_t *)args_ptr - 8;
        gpr_ptr = vec_ptr - 8;

#ifdef RG_DEBUG
        memset(gpr_ptr, 0, top_ptr - (uint8_t *)gpr_ptr);
#endif
    } else {
        return_ptr = top_ptr - AlignLen(func->ret.type->size, 16);

        args_ptr = return_ptr - func->args_size;
        vec_ptr = (uint64_t *)args_ptr - 8;
        gpr_ptr = vec_ptr - 8;

#ifdef RG_DEBUG
        memset(gpr_ptr, 0, top_ptr - (uint8_t *)gpr_ptr);
#endif

        gpr_ptr[gpr_count++] = (uint64_t)return_ptr;
    }

    RG_ASSERT(AlignUp(lib->stack.ptr, 16) == lib->stack.ptr);
    RG_ASSERT(AlignUp(lib->stack.end(), 16) == lib->stack.end());
    RG_ASSERT(AlignUp(args_ptr, 16) == args_ptr);

    // Push arguments
    for (Size i = 0; i < func->parameters.len; i++) {
        const ParameterInfo &param = func->parameters[i];
        Napi::Value value = info[i];

        switch (param.type->primitive) {
            case PrimitiveKind::Void: { RG_UNREACHABLE(); } break;

            case PrimitiveKind::Bool: {
                if (!value.IsBoolean()) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected boolean", GetTypeName(value.Type()));
                    return env.Null();
                }

                bool b = info[i].As<Napi::Boolean>();

                if (RG_LIKELY(gpr_count < 8)) {
                    gpr_ptr[gpr_count++] = (uint64_t)b;
                } else {
                    *args_ptr = (uint8_t)b;
                    args_ptr += 1;
                }
            } break;
            case PrimitiveKind::Int8:
            case PrimitiveKind::UInt8:
            case PrimitiveKind::Int16:
            case PrimitiveKind::UInt16:
            case PrimitiveKind::Int32:
            case PrimitiveKind::UInt32:
            case PrimitiveKind::Int64:
            case PrimitiveKind::UInt64: {
                int64_t v;
                if (!CopyNodeNumber(info[i], &v))
                    return env.Null();

                if (RG_LIKELY(gpr_count < 8)) {
                    gpr_ptr[gpr_count++] = (uint64_t)v;
                } else {
                    args_ptr = AlignUp(args_ptr, param.type->align);
                    memcpy(args_ptr, &v, param.type->size); // Little Endian
                    args_ptr += param.type->size;
                }
            } break;
            case PrimitiveKind::Float32: {
                float f;
                if (!CopyNodeNumber(info[i], &f))
                    return env.Null();

                if (RG_LIKELY(vec_count < 8)) {
                    memcpy(vec_ptr + vec_count++, &f, 4);
                } else {
                    args_ptr = AlignUp(args_ptr, 4);
                    memcpy(args_ptr, &f, 4);
                    args_ptr += 4;
                }
            } break;
            case PrimitiveKind::Float64: {
                double d;
                if (!CopyNodeNumber(info[i], &d))
                    return env.Null();

                if (RG_LIKELY(vec_count < 8)) {
                    memcpy(vec_ptr + vec_count++, &d, 8);
                } else {
                    args_ptr = AlignUp(args_ptr, 8);
                    memcpy(args_ptr, &d, 8);
                    args_ptr += 8;
                }
            } break;
            case PrimitiveKind::String: {
                if (!value.IsString()) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected string", GetTypeName(value.Type()));
                    return env.Null();
                }

                const char *str = CopyNodeString(info[i], &lib->tmp_alloc);

                if (RG_LIKELY(gpr_count < 8)) {
                    gpr_ptr[gpr_count++] = (uint64_t)str;
                } else {
                    args_ptr = AlignUp(args_ptr, 8);
                    *(uint64_t *)args_ptr = (uint64_t)str;
                    args_ptr += 8;
                }
            } break;

            case PrimitiveKind::Record: {
                if (!value.IsObject()) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected object", GetTypeName(value.Type()));
                    return env.Null();
                }

                Napi::Object obj = info[i].As<Napi::Object>();

                if (param.gpr_count || param.vec_count) {
                    RG_ASSERT(param.type->size <= 16);
                    RG_ASSERT(param.gpr_count <= 8 - gpr_count);
                    RG_ASSERT(param.vec_count <= 8 - vec_count);

                    uint64_t buf[2] = {};
                    if (!PushObject(obj, param.type, &lib->tmp_alloc, (uint8_t *)buf))
                        return env.Null();

                    if (param.gpr_first) {
                        uint64_t *ptr = buf;

                        gpr_ptr[gpr_count++] = *(ptr++);
                        if (param.gpr_count == 2) {
                            gpr_ptr[gpr_count++] = *(ptr++);
                        } else if (param.vec_count == 1) {
                            vec_ptr[vec_count++] = *(ptr++);
                        }
                    } else {
                        uint64_t *ptr = buf;

                        vec_ptr[vec_count++] = *(ptr++);
                        if (param.vec_count == 2) {
                            vec_ptr[vec_count++] = *(ptr++);
                        } else if (param.gpr_count == 1) {
                            gpr_ptr[gpr_count++] = *(ptr++);
                        }
                    }
                } else if (param.type->size) {
                    args_ptr = AlignUp(args_ptr, param.type->align);
                    if (!PushObject(obj, param.type, &lib->tmp_alloc, args_ptr))
                        return env.Null();
                    args_ptr += param.type->size;
                }
            } break;

            case PrimitiveKind::Pointer: {
                if (!value.IsExternal()) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected external", GetTypeName(value.Type()));
                    return env.Null();
                }

                void *ptr = info[i].As<Napi::External<void>>();

                if (RG_LIKELY(gpr_count < 8)) {
                    gpr_ptr[gpr_count++] = (uint64_t)ptr;
                } else {
                    args_ptr = AlignUp(args_ptr, 8);
                    *(uint64_t *)args_ptr = (uint64_t)ptr;
                    args_ptr += 8;
                }
            } break;
        }
    }

    // DumpStack(func, MakeSpan((uint8_t *)gpr_ptr, top_ptr - (uint8_t *)gpr_ptr));

#define PERFORM_CALL(Suffix) \
    (vec_count ? ForwardCallX ## Suffix(func->func, (uint8_t *)gpr_ptr) \
               : ForwardCall ## Suffix(func->func, (uint8_t *)gpr_ptr))

    // Execute and convert return value
    switch (func->ret.type->primitive) {
        case PrimitiveKind::Float32: {
            float f = PERFORM_CALL(F);

            return Napi::Number::New(env, (double)f);
        } break;

        case PrimitiveKind::Float64: {
            D0X0Ret ret = PERFORM_CALL(DG);

            return Napi::Number::New(env, (double)ret.d0);
        } break;

        case PrimitiveKind::Record: {
            if (func->ret.gpr_first && !func->ret.vec_count) {
                X0X1Ret ret = PERFORM_CALL(GG);

                Napi::Object obj = PopObject(env, (const uint8_t *)&ret, func->ret.type);
                return obj;
            } else if (func->ret.gpr_first) {
                X0D0Ret ret = PERFORM_CALL(GD);

                Napi::Object obj = PopObject(env, (const uint8_t *)&ret, func->ret.type);
                return obj;
            } else if (func->ret.vec_count) {
                D0X0Ret ret = PERFORM_CALL(DG);

                Napi::Object obj = PopObject(env, (const uint8_t *)&ret, func->ret.type);
                return obj;
            } else if (func->ret.type->size) {
                RG_ASSERT(return_ptr);

                X0X1Ret ret = PERFORM_CALL(GG);
                RG_ASSERT(ret.x0 = (uint64_t)return_ptr);

                Napi::Object obj = PopObject(env, return_ptr, func->ret.type);
                return obj;
            } else {
                PERFORM_CALL(GG);

                Napi::Object obj = Napi::Object::New(env);
                return obj;
            }
        } break;

        default: {
            X0X1Ret ret = PERFORM_CALL(GG);

            switch (func->ret.type->primitive) {
                case PrimitiveKind::Void: return env.Null();
                case PrimitiveKind::Bool: return Napi::Boolean::New(env, ret.x0);
                case PrimitiveKind::Int8: return Napi::Number::New(env, (double)ret.x0);
                case PrimitiveKind::UInt8: return Napi::Number::New(env, (double)ret.x0);
                case PrimitiveKind::Int16: return Napi::Number::New(env, (double)ret.x0);
                case PrimitiveKind::UInt16: return Napi::Number::New(env, (double)ret.x0);
                case PrimitiveKind::Int32: return Napi::Number::New(env, (double)ret.x0);
                case PrimitiveKind::UInt32: return Napi::Number::New(env, (double)ret.x0);
                case PrimitiveKind::Int64: return Napi::BigInt::New(env, (int64_t)ret.x0);
                case PrimitiveKind::UInt64: return Napi::BigInt::New(env, ret.x0);
                case PrimitiveKind::Float32: { RG_UNREACHABLE(); } break;
                case PrimitiveKind::Float64: { RG_UNREACHABLE(); } break;
                case PrimitiveKind::String: return Napi::String::New(env, (const char *)ret.x0);

                case PrimitiveKind::Record: { RG_UNREACHABLE(); } break;

                case PrimitiveKind::Pointer: {
                    void *ptr = (void *)ret.x0;
                    return Napi::External<void>::New(env, ptr);
                } break;
            }
        } break;
    }

#undef PERFORM_CALL

    RG_UNREACHABLE();
}

}

#endif
