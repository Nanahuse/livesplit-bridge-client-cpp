# C# serialization cases

`csharp-serialized-cases.json` contains JSON produced from the official
LiveSplit.Bridge C# DTOs at commit
`4c14c0161eacd50b05411c0d20f5eff523e9c155`.

It is used only to verify that the generated C++ models accept and reproduce the
official wire representation. The pinned source metadata in
`tools/protocol-source.json` remains the source of truth for the protocol; the
official JSON Schemas are fetched at generation time and are not stored in this
repository.

The original samples come from the LiveSplit.Bridge project
(<https://github.com/Nanahuse/LiveSplit.Bridge>), licensed under the MIT License.
