# project_cli

Repo-local file-copy CLI example for Nebula 1.0's primary tooling story.

Run it with:

```bash
printf 'nebula\n' > /tmp/project-cli.in
./build/nebula run examples/project_cli --run-gate none -- /tmp/project-cli.in /tmp/project-cli.out
cat /tmp/project-cli.out
```
