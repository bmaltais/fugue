# Fugue

Node-based workflows for local AI on Omarchy, in C++ and Qt Quick.

Wire steps together (a prompt, an image, an edit, a clip, a song) and run the graph on the
models [omarchy-local-ai](https://github.com/omacom/omarchy-local-ai) serves. Add your own
nodes as a folder with a `node.json` and a program, in any language.

**Status: designed, not built.** The window is still the starter counter app. The design is
in [docs/spec.md](docs/spec.md), the vocabulary in [docs/GLOSSARY.md](docs/GLOSSARY.md), and
the work is tracked in the [issues](https://github.com/bmaltais/fugue/issues) by milestone.

## Build

```sh
bin/build
bin/test
```

## License

MIT.
