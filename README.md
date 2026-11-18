# Fugue

Node-based workflows for local AI on Omarchy, in C++ and Qt Quick.

Wire steps together (a prompt, an image, an edit, a clip, a song) and run the graph on the
models [omarchy-local-ai](https://github.com/omacom/omarchy-local-ai) serves. Add your own
nodes as a folder with a `node.json` and a program, in any language.

**Status: just scaffolded.** The window is still the starter counter app. The plan lives in
the design note, not here.

## Build

```sh
bin/build
bin/test
```

## License

MIT.
