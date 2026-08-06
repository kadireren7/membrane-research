# Primary email template (general-purpose first contact)

The single strongest, most general version — use this when a recipient
doesn't cleanly fit one of the four category-specific templates in
`outreach/email-templates.md`, or as a starting point to customize
further. Roughly 150 words. Same rules as that file: fill in every
bracketed field with something real and verified before sending; never
send with a placeholder still in it; nothing here has been sent yet.

---

**Subject:** KV-cache research project — looking for FPGA/CXL access

Hi [Name],

I'm Kadir Eren Altıntaş, a systems programming student and the creator
of MEMBRANE, an open-source research project on LLM KV-cache memory:
mixed-precision tiering verified bit-exact against ggml's reference
quantizer, exact (non-approximate) sparse retrieval, and a
synthesizable FPGA quantization datapath cosimulated against the same
reference math — 520,000 transactions, zero mismatches. Everything
hardware-adjacent in the project today is simulation or a synthesis
cell-count check, not a real result, and that's the gap I'm trying to
close.

I came across [specific paper/project], and [why it connects] made me
think this might be relevant to your work. I'm looking for
[requested hardware/platform] — even limited or remote access — to run
a real place-and-route (and, if that goes well, a board bring-up).
Full scope: `docs/phase8-hardware-validation-plan.md`.

Repository: github.com/kadireren7/membrane

Kadir
