# Target selection framework

Criteria for identifying which labs/teams to reach out to, deliberately
without naming specific people or organizations here. **If a real target
list is built from this framework, it requires its own separate
verification pass against current, official sources** (lab websites,
recent publication lists, current staff pages) — nothing in this
document should be treated as a ready-to-use contact list, and no
organization or individual is named or implied as already contacted.

## Categories

### 1. Computer architecture labs

Look for: recent (last 2-3 years) publications on memory systems,
near-memory/processing-in-memory computing, or KV-cache/LLM-serving
hardware — the closest analogue to this project's own related-work
comparison (`paper/related-work-matrix.md`'s TRACE/PNM-CXL entries).

### 2. ML systems labs

Look for: publications on KV-cache management, quantization, or LLM
serving systems specifically (the H2O/Scissorhands/KIVI/KVQuant/Quest/
PagedAttention/FlexGen/InfiniGen family in
`paper/related-work-matrix.md`) — labs already working in exactly this
problem space are the most likely to find the exact-retrieval framing
interesting, favorably or critically.

### 3. Memory systems / CXL labs

Look for: groups with a named CXL research thread, or affiliation with
the CXL Consortium, or recent work on memory disaggregation/pooling.

### 4. FPGA labs

Look for: groups with an active FPGA fabrication/bring-up practice (not
just FPGA-as-simulation-target) — evidence of real board results in
recent publications, not just RTL design papers.

### 5. Cloud FPGA programs

Look for: cloud providers or university programs offering FPGA
instance/dev-kit access (e.g. F1-class cloud FPGA instances, or a
university's shared FPGA cluster) — lower barrier to a scoped Level A/B
experiment than requiring a lab to own physical hardware outright.

### 6. Semiconductor research teams

Look for: industry research groups (not product teams) with a stated
open-research or academic-collaboration mandate, particularly ones
publishing openly (arXiv preprints, not just internal reports) — a
signal that external collaboration is culturally normal for that team.

## Selection criteria (apply within each category)

| Criterion | Why it matters |
|---|---|
| **Real FPGA access** | Table stakes for Level A/B — no point reaching out to a group with no board/toolchain access at all. |
| **CXL platform access** | Rare; a group that has this is disproportionately valuable for Level C specifically. |
| **Published memory-systems work** | Signals the group will understand the technical content quickly and can give substantive (not just logistical) feedback. |
| **Student collaboration openness** | Groups that run student capstone/thesis projects are often a lower-friction path than requesting a PI's own time. |
| **Open-source research practice** | Groups that already publish code/data openly are more likely to engage with a project whose entire premise is disclosure and reproducibility. |
| **Geographic/remote feasibility** | Remote board access (SSH to a lab machine, or a cloud FPGA instance) removes the biggest practical blocker — prioritize groups known to support this over groups requiring in-person lab access only. |

## Process note

Building an actual named list from this framework is explicitly **out
of scope for this document** — see the warning at the top. When that
list is built, each entry should independently cite the specific,
current, official source (lab webpage, recent paper, program page) that
justified including it, dated at the time of verification, since lab
staffing/focus/access changes over time.
