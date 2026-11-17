# Tooling and checks

This page lists how to check a change before it lands, and how the docs are built and checked.

## Before you contribute

Read [Architecture](../concepts/architecture.md): section 0 lists the hard rules. Turn on the hooks of `.pre-commit-config.yaml`:

```sh
pre-commit install --hook-type pre-commit --hook-type pre-push
```

`scripts/ci/all.sh` runs every CI job that this computer can run, in parallel, with the scripts that the workflows call.

## The docs

The pages are Markdown, the README and the `docs` folder and the `README.md` of each folder that [the site outline](../nav.md) lists. [docgen](https://docgen.mcswain.dev/) fills the generated facts: a marker holds a fact that comes from the repository, such as an excerpt of a file, a constant, the usage of a binary or the settings table, and its body is never edited by hand.

<!-- docgen:text text="go run github.com/USA-RedDragon/docgen@{docgen} update" code=sh -->
```sh
go run github.com/USA-RedDragon/docgen@v0.0.0-20261117171836-9969cd79f7f3 update
```
<!-- /docgen:text -->

<!-- docgen:text text="go run github.com/USA-RedDragon/docgen@{docgen} check -diff" code=sh -->
```sh
go run github.com/USA-RedDragon/docgen@v0.0.0-20261117171836-9969cd79f7f3 check -diff
```
<!-- /docgen:text -->

`check` fails and lists each stale marker, each unused region and each broken link of a page under `docs`.

`docs/site/build.sh` builds the pages into a static site with pandoc; `scripts/ci/docs.sh` runs docgen, installs the pinned pandoc and builds. The build fails on a page that is in the outline and not written, a Markdown page under `docs` that is not in the outline, and any link, image or heading anchor of a built page that does not resolve.
