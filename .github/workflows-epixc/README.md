# ePixC firmware CI

Not in `.github/workflows/` on purpose. This is a **fork** of WLED, and upstream ships its own
workflows there. Adding a file to that directory means every future upstream merge has a conflict
in it, and merge conflicts in CI config are the kind that get resolved carelessly.

Move `build.yaml` into `.github/workflows/` when the fork is ready to diverge from upstream CI, or
add it as a second workflow file with a name upstream will never use.
