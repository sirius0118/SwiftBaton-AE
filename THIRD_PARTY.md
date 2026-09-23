# Third-party sources

The repository includes modified copies of the following projects. Preserve their component-specific licenses and copyright notices when redistributing them. No single blanket license replaces the licenses in these source trees.

| Component | Source baseline |
| --- | --- |
| CRIU | `5c63f584e4f58a05d290f9f3dc188f63ad08175c` |
| Fluid | `72b1ab2d8cbebbfc85080539048145292c55e309` |
| YCSB | `74d013849c3c9a6111e66bdf85ddeffb482d64be` |
| Docker CE CLI / engine | CLI baseline `44a430f4c43e61c95d4e9e9fd6a0573fa113a119`; engine source from the SwiftBaton source backup `30ebc00dfcd9bb78635079a37c85b22884da7075` |
| runc | `e112c95b30e5fc3394d27e96a3b7573e778c1a63` |
| containerd | `643fa70a7d7716e1e8138a3f2b2ce0532886676c` |

Local implementation changes are included directly in the source files. Vendored dependencies retain their upstream notices. The protobuf descriptor under `criu/images/google/protobuf/` retains its original copyright and license header.
