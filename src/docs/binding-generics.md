# Binding generics

This document explores projections of Swift generics into C#.

1. [Generic functions](#generic-functions)
2. Generic types (TBD)
3. Generics with protocol constraints (TBD)
4. Generic with PAT constraints (TBD)

## Generic functions

When a generic type is used in Swift, its memory layout isn't known at compile time. To manage this, the method parameter whose type is genericis represented by an opaque pointer. To handle this:

- **Opaque Pointers**: Swift represents generic parameters as opaque pointers (`%swift.opaque`), abstracting the actual memory layout.
- **Type Metadata**: Swift includes a type metadata pointer (`%T`) as an implicit argument at the end of a function's signature. This metadata provides the runtime information needed to manage the generic type.
- **Indirect Return**: For returning generic types, Swift uses an [indirect return result](https://github.com/dotnet/runtime/issues/100543), where the caller provides a memory location for the return value.

Consider the following simple swift function:

```swift
func returnData<T>(data: T) -> T {
    return data
}
```

When compiled into LLVM-IR the above function has the following LLVM-IR signature:

```llvm
@"output.returnData<A>(data: A) -> A"(ptr noalias nocapture sret(%swift.opaque) %0, ptr noalias nocapture %1, ptr %T)
```

- `%0`: Indicates an **indirect return result**, where the caller allocates memory for the return value.
- `%1`: The `data` parameter, passed as a pointer.
- `%T`: The **type metadata** for `T`.

### Projecting into C\#

A sample projection into C# assuming that the type argument is projected as struct could look like this:

```csharp
[DllImport(Path, EntryPoint = "...")]
[UnmanagedCallConv(CallConvs = [typeof(CallConvSwift)])]
public static extern void PInvoke_ReturnData(SwiftIndirectResult result, void* data, void* metadata);

public static unsafe T ReturnData<T>(T data) where T : unmanaged, ISwiftObject
{
    var metadata = Runtime.GetMetadata(data);
    nuint payloadSize = /* Extract size from metadata */;
    
    
    var payload = NativeMemory.Alloc(payloadSize);
    try 
    {
        var result = new SwiftIndirectResult(payload);
        PInvoke_ReturnData(result, &data, metadata);
        return *(T*)payload;
    }
    finally
    {
        NativeMemory.Free(payload);
    }
}
```

However, the projection has to support type arguments agnostic of whether they are projected as structs (frozen structs) or classes (non-frozen structs). We cannot then use the `unmanaged` constraint on `T` and will need to introduce abstractions for getting a native handle of an arbitrary Swift type and constructing a Swift type from such a handle.

Those could look something like

```csharp
public static unsafe void* GetNativeHandle<T>(T type) where T : ISwiftObject
public static unsafe T ConstructFromNativeHandle<T>(void* payload) where T : ISwiftObject
```

Using these abstractions, our projection becomes:

```csharp
[DllImport(Path, EntryPoint = "...")]
[UnmanagedCallConv(CallConvs = [typeof(CallConvSwift)])]
public static extern void PInvoke_ReturnData(SwiftIndirectResult result, void* data, void* metadata);

public static unsafe T ReturnData<T>(T data) where T : ISwiftObject
{
    var metadata = Runtime.GetMetadata(data);
    nuint payloadSize = /* Extract size from metadata */;
    var payload = NativeMemory.Alloc(payloadSize);
    try 
    {
        SwiftIndirectResult result = new SwiftIndirectResult(payload);
        
        void* dataPointer = Runtime.GetNativeHandle<T>(ref data)
        PInvoke_ReturnData(result, dataPointer, metadata);
        return Runtime.ConstructFromNativeHandle<T>(payload);
    }
    finally
    {
        NativeMemory.Free(payload);
    }
}
```

**Memory Management Considerations:**

- **Structs**: The CLR copies the data from unmanaged to managed memory. We must free the unmanaged memory to avoid leaks.
- **Classes**: In this example we expect that calling `ConstructFromNativeHandle` will copy the necessary data so that the function can free all the resources it allocated.

#### New API implementation

##### `GetNativeHandle` Implementation

We can implement `GetNativeHandle` by extending the `ISwiftObject` interface:

```csharp
static unsafe void* GetNativeHandle<T>(ref T type) where T : ISwiftObject
{
    return type.GetNativeHandle();
}

unsafe interface ISwiftObject
{
    // Other members
    
    public unsafe void* GetNativeHandle();
}

unsafe class NonFrozenStruct : ISwiftObject
{
    private void* _payload;

    // Other members

    public unsafe void* GetNativeHandle() => _payload;
}

unsafe struct FrozenStruct : ISwiftObject
{
    // Other members

    public unsafe void* GetNativeHandle() => Unsafe.AsPointer(ref this);
}
```

##### `FromNativeHandle` Implementation

We can make `ISwiftObject` generic and include a factory method:

```csharp
static unsafe T FromPayload<T>(void* payload) where T : ISwiftObject<T>
{
    return T.FromNativeHandle(payload);
}

public unsafe interface ISwiftObject<T> where T : ISwiftObject<T>
{
    // Other members

    public static abstract unsafe T FromNativeHandle(void* payload);
}

unsafe class NonFrozenStruct : ISwiftObject<NonFrozenStruct>
{
    private static nuint PayloadSize = /* Extract size from metadata */;
    private void* _payload;
    
    private NonFrozenStruct(void* payload) { 
        _payload = NativeMemory.Alloc(PayloadSize); 
        NativeMemory.Copy(payload, _payload, PayloadSize);
    }
    
    // Other members
    
    public static unsafe NonFrozenStruct FromNativeHandle(void* payload)
    {
        return new NonFrozenStruct(payload);
    }
}

unsafe struct FrozenStruct: ISwiftObject<FrozenStruct>
{
    // Other members

    public static unsafe FrozenStruct FromNativeHandle(void* payload) => *(FrozenStruct*)payload;
}
```

An alternative to extending the `ISwiftObject` interface with new methods could be using reflection.

**TODO:**

- Figure out how to handle builtin types e.g. Int
