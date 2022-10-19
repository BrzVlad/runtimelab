﻿// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Text.Json.Serialization.Tests
{
    public sealed partial class NodeInteropTestsDynamic : NodeInteropTests
    {
        public NodeInteropTestsDynamic() : base(JsonSerializerWrapperForString.StringSerializer) { }
    }
}
