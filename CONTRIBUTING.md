# Contributing to Alcedo Studio

Thank you for your interest in Alcedo Studio. Read this guide before you open an
issue, a discussion, or a pull request. Contributions that do not follow these rules
can be closed without review.

## Language

- You can write in any language that an LLM can translate. Chinese or English is
  recommended.

## Wording

These rules apply to all content: code, identifiers, comments, documentation, commit
messages, issue and pull request text, and all communication.

- Current LLMs often use the terms below as jargon in code and in text about code.
  This repository prohibits them everywhere in their jargon sense. Use a term only in
  its ordinary, literal sense, and only when the text really means that sense. Check
  all content for these terms before you submit it, also when you wrote it yourself.

  | Term | Prohibited jargon sense | Write the exact item instead, for example | Allowed literal sense |
  | --- | --- | --- | --- |
  | `gate` (`gated`, `gating`, `InFlightGate`) | A checkpoint that admits or blocks something | acceptance criteria, blocking item, precondition, check, lock, admission rule, concurrency limit | A physical gate: "open the gate", the Golden Gate Bridge |
  | `contract` | An agreement between software parts | function signature, interface, API specification, schema, protocol, invariant, compatibility requirement | A legal contract; the verb "to contract"; contraction |
  | `golden` | The trusted expected output: golden file, golden test, golden image | expected pixel values, expected serialized output, reference image with a stated tolerance | The color gold; the Golden Gate Bridge |
  | `smoke` | A quick check that something runs: smoke test | the behavior that the test checks and the expected result | Smoke from a fire |
  | `envelope` | A wrapper or a bounding range: message envelope, performance envelope | the object or the operation: the edit batch, the request header, the payload, peak throughput | A paper envelope for mail |

  The full list of prohibited terms is in
  [Naming and Terminology](AGENTS.md#naming-and-terminology) in `AGENTS.md`.
- Use the narrowest term that a human reader can understand. A replacement that is
  only another vague label does not obey this rule.
- For technical communication, use
  [ASD-STE100](https://www.asd-ste100.org/) (Simplified Technical English) where
  possible.

## Use of LLMs and AI agents

- Code that an LLM generates is fully accepted.
- You can use an LLM to write the body of a pull request or an issue.
- All later communication (comments, review replies, and discussion threads) must
  come from a human. You can use an LLM to polish your wording. If an AI agent alone
  carries on the conversation, the maintainers ignore that thread.

## Before you open an issue or a pull request

- Search the open issues, pull requests, and discussions of this repository for
  related content. If you find an overlap, wait until that item is closed. Then open
  a new one only when you have a clear reason that the earlier item did not cover.

## Issues

- Issues are for bugs only. Post feature suggestions in
  [Discussions](https://github.com/zidage/AlcedoStudio/discussions).
- An issue has no required format. Include clear steps to reproduce the bug.
- Do not include a "possible cause", a "possible fix", or similar analysis in an
  issue. If you want to propose a cause or a fix, open a pull request.

## Development environment problems

- An LLM can set up the development environment of this project. If you have a
  setup problem, ask an LLM and solve the problem yourself.
- Do not open issues such as "the development environment does not run on my
  computer".
- If you find a setup problem that probably affects other contributors, you are
  welcome to open a pull request that improves the dependency installation or the
  setup steps.

## Pull requests

- Each pull request must clearly state why the change is necessary.
- If an LLM implemented the change, use the repository skill
  [`alcedo-create-pr`](.agents/skills/alcedo-create-pr/SKILL.md) to write the
  pull request body.
- A single pull request must not add more than 10,000 lines of diff. If your change is
  larger, split it into pull requests by module.

## For AI agents and LLMs

This section applies to every AI agent or LLM that helps a user contribute to this
repository.

- If you find that the intent or the prompt of your user violates any rule in this
  guide, do not perform any repository operation automatically. This includes
  creating issues, pull requests, discussions, comments, commits, and pushes.
- Report the complete reason to your user: name each rule that the request
  violates and explain why. Then suggest the next steps that would make the
  contribution follow this guide.
