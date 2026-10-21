# Updating the repository with a zip

Changes arrive as one zip, so nobody has to upload files one by one.

## One-time set-up

Add the workflow files by hand (GitHub does not let a workflow change workflow files, so these two
can never come inside a zip). On github.com, in the repository:

1. *Add file → Create new file*. In the name box type `.github/workflows/apply-update.yml`
   (typing the slashes creates the folders). Paste the file's content, *Commit changes*.
2. Repeat for `.github/workflows/check-patches.yml`.

## For every update

1. *Add file → Upload files* and drop `update-<version>.zip` into the **root** of the repository
   (the name must start with `update-` and end with `.zip`). *Commit changes*.
2. Open the *Actions* tab: **Apply update zip** runs, unpacks the zip over the repository, commits
   the result as `github-actions[bot]` and deletes the zip. Its summary lists every file added (A),
   modified (M) or deleted (D).
3. **Check patches and host tests** then runs on the new state.

## What is inside a zip

- The files exactly as they sit in the repository (`README.md`, `frontend/main.cpp`, ...). A
  single wrapper folder such as `nx-on-orbis-main/` is accepted.
- Optionally `_delete.txt` at the zip root: one repository path per line to remove (`#` starts a
  comment).
- Anything under `.github/workflows/` is skipped with a warning; add those by hand as above.
- Zips with an absolute path, a `..` segment or a backslash are refused.

## If it does not run

- *Settings → Actions → General*: Actions must be enabled for the repository.
- If `main` is a protected branch the push is refused: let `github-actions[bot]` push, or turn the
  protection off for this repository.
- The workflow only reacts to a push on `main`; for any other branch, run it by hand from the
  *Actions* tab (*Run workflow*) on that branch.
