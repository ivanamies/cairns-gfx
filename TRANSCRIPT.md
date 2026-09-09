# TRANSCRIPT: neurofly integration

Background conversation that motivated the `ia/2026-09/ml/neurofly_integration`
branch, reproduced verbatim as pasted by the owner on 2026-09-09. A log of what
was actually built on that date follows at the end.

---

> so I'm about to embark on a silly project
> I'm going to port https://github.com/NeLy-EPFL/NeuroMechFly
> into C++
> and into my editor engine that creates training data
> and then I'm going to try to replace whatever is inside the fly with a small vision language model
> do you think it will work

Short version: yes, but only if "replace whatever is inside the fly" means replacing the brain, not the spinal cord. A few things worth knowing before you start.

You've linked the wrong repo. That one was archived in October 2024 and is the legacy PyBullet v1 code; the maintained version is FlyGym, the Python library for NeuroMechFly v2, a digital twin of the fruit fly that can see, smell, walk over challenging terrain, and interact with the environment. That matters for your plan because v2 already has the thing you need: a color camera attached to each compound eye, with each image transformed into 721 bins on a hexagonal grid, plus olfaction, leg adhesion, and a Gymnasium interface. Start from flygym.

The C++ port is smaller than it sounds — or much bigger, depending on your engine. v2 is implemented on MuJoCo and dm_control, and MuJoCo is a C library with a fine C++ API. The fly itself is an MJCF XML plus meshes, which load directly into C++ MuJoCo with zero porting. What you'd actually port is the Python layer: retina projection, the CPG/hybrid locomotion controller, arena logic, observation packing. That's a few thousand lines and quite doable. If, however, your editor engine has its own physics and you want the fly to live there, you're re-tuning an ~87-joint articulated body at 1000× scale with adhesion actuators and stiff contacts on thin legs. That's where months go. My strong suggestion: embed MuJoCo in your engine rather than reimplementing the body.

Now, the VLM. Three versions of your idea, in order of doom:

1. VLM outputs joint angles every physics step. Won't work. The control loop runs at sub-millisecond timesteps with 42 leg DoF that need tight phase coordination; a VLM runs at a few Hz with fuzzy numeric output. You get a twitching corpse.
2. VLM outputs descending commands (forward drive, turn bias, maybe "groom"/"stop"), and the existing CPG-based controller handles the legs. This is exactly the abstraction v2 built: a descending interface between higher-level sensing and lower-level locomotion. The VLM becomes the brain, the controller is the ventral nerve cord. This can work and is a fun experiment.
3. Same as 2, but with the VLM fine-tuned on trajectories from your training-data engine rather than used zero-shot. This is the version that will actually work. A small VLM looking at a 721-pixel hex retina image zero-shot will mostly guess; give it a few hundred thousand (retina, proprioception, target command) samples distilled from a scripted or RL expert and it becomes plausible.

Practical traps: run the VLM async with a command buffer so the physics never blocks on inference; decide early whether you feed the biologically faithful hex retina or a normal rendered camera (the latter is far friendlier to a pretrained vision encoder); and pick a task with a clear expert — v2 already demonstrates odor-plume navigation and fly-following, which give you ready-made benchmarks and reference controllers to distill from.

So: silly, yes. Doomed, no — as long as the language model is giving orders, not moving legs.

> so did they ever figure out mech interp for the fly brain?
> and what are the chances do you think I can steal whatever mech interp they did do on the fly brain for whatever I'm trying to do

Depends which "mech interp" you mean, because the fly people use the phrase differently from the SAE crowd.

The neuroscience version: partially, and less than the headlines suggest. The 2024 FlyWire connectome gave the wiring diagram, and a few groups have tried to turn wiring into mechanism:

- Shiu et al. built a leaky integrate-and-fire model of the whole brain that reproduces things like sugar-sensitive neurons in the mouth detecting something sweet, activating downstream neurons, some of which stimulate motor neurons to extend the proboscis. That's a real sensorimotor circuit falling out of pure connectivity, which was a big deal.
- Lappalainen et al. (flyvis) built a deep mechanistic network with experimentally measured connectivity and unknown parameters for single-neuron dynamics and synapse strength, optimized on a visual-motion task, and it predicts the ON/OFF channel segregation and the direction selectivity of the T4 and T5 motion detectors.
- Pugliese et al. simulated the ventral nerve cord connectome to identify a minimal three-neuron central pattern generator for walking, confirmed by optogenetic experiments.
- The "effectome" paper is the most mech-interp-flavored in spirit: it proposes a linear dynamical model of the fly brain using stochastic optogenetic perturbation data to estimate causal effects, with the connectome as a prior, and finds the dynamics are high-dimensional and supported by many small independent circuits.

The caveat that matters for you: an April 2026 preprint retrained flyvis-style ensembles and found that while task performance stays similar across models, the identification of biologically plausible circuit solutions is unstable across retraining runs. So the connectome constrains, but it doesn't identify. Nobody has "solved" the fly brain; they've got a handful of well-validated subcircuits and a lot of plausible-but-unverified whole-brain dynamics. (Also, a male CNS connectome just landed this month, so the datasets are still moving.)

The ML version (SAEs, probes, circuit discovery on a trained model): essentially nobody has done it on the fly brain models. The connectome-constrained nets are considered interpretable by construction, since every unit is a named neuron, so the field hasn't needed the toolkit.

Can you steal it? Yes, and the best theft is obvious once you see it. Flygym's retina has 721 hexagonal bins per eye, and flyvis models roughly 45,000 neurons across 721 columns of the central visual field. Same lattice. That's not a coincidence; NeuroMechFly v2's fly-following demo already plugged a connectome-constrained visual network into the fly. So: run flyvis (it's PyTorch, open on GitHub) as a frozen front end and feed your VLM the T4/T5 motion signals and downstream activations instead of raw pixels. You get a biologically-grounded feature extractor for free, and a VLM that's reasoning over "what the fly's optic lobe sees" rather than over a 721-pixel blob it was never trained on.

Second-best theft: FlyGM (Feb 2026) instantiates the whole-brain connectome as a graph-structured neural controller for a simulated biomechanical fly via deep RL, with observations mapped to afferent neurons, propagated through connectome-constrained message passing, and efferent states decoded into motor actions in MuJoCo. Code's public. That is quite literally "whatever is inside the fly," already in MuJoCo, and it's a more principled low-level controller for your VLM to sit on top of than the hand-tuned CPGs.

Not worth stealing: the effectome (it's an experimental design that needs optogenetic data you don't have) and the LIF whole-brain sims (spiking at 5 kHz is a bad neighbor for a VLM running at 5 Hz).

One inversion worth noticing: the reproducibility result means nobody actually knows which internal solution a trained fly-visual-net converges to. If you fine-tune a VLM to drive the fly and then run actual ML mech interp on it, comparing what the VLM learned against the T4/T5 channels would be a legitimately novel experiment. Your silly project has a serious project hiding inside it.

> [on C. elegans]

Mapped, yes. Understood, no — and the worm is the cleanest demonstration that a wiring diagram isn't an explanation.

The map itself is done: the complete structure and connectivity of the C. elegans nervous system was first published in 1986 — 302 neurons, roughly 7,000 chemical synapses, ~600 gap junctions — and it's since been redone with modern EM for both sexes and across development. That's what you saw in college and it hasn't been superseded, only refined.

The problem is what happened next, which is roughly nothing for thirty years. Physical connectivity patterns of the C. elegans nervous system are unable to predict how the nervous system functions as a whole to enact behaviors. OpenWorm has been trying to build the worm bottom-up since ~2011 and still can't get behavior out of the connectome from first principles. The reasons are instructive:

- Most worm neurons don't spike; they're graded, analog, and slow. The "wire" model is wrong at the physical layer.
- Gap junctions and, much worse, neuropeptides. When the neuropeptide "wireless" network was mapped in 2023, it turned out to be denser than the synaptic one. The EM connectome literally doesn't show most of the signaling.
- Synapse signs and weights aren't in the anatomy. When the Leifer lab optogenetically poked neurons one at a time and imaged the whole brain (Randi et al., 2023), the anatomical connectome predicted the functional responses poorly.

What's actually changed recently is the effectome idea I mentioned for the fly got done in the worm. A 2025 follow-up fit a connectome-constrained dynamical model to whole-brain recordings during single-neuron optogenetic perturbation; with non-zero weights only between anatomically connected neurons, it captured causal interactions between all pairs 92% as well as the reproducibility of the perturbation data themselves, including interactions between anatomically unconnected neurons, explained by signal propagation over multi-neuron paths. So the wiring does explain function — but only once you've fit the weights against perturbation data. The anatomy alone was never going to do it. Meanwhile Flavell's lab showed behavioral information is expressed across the brain with distinct tunings, timescales and flexibility, and nearly 60% of head neurons encode at least one behavior — so even a 302-neuron brain is a distributed, mixed-selectivity mess rather than a tidy circuit board.

Here's the part that's counterintuitive: the fly's optic lobe is arguably better understood than the worm's brain, despite having 100× more neurons. Understanding tracks circuit structure, not neuron count. The optic lobe is repeated, largely feedforward, high-throughput, and has a clean computational question (motion) with a clean answer (T4/T5). The worm's 302 neurons are each multifunctional, neuromodulated, and operating on several timescales at once. Going "lower" doesn't buy you simplicity; it buys you a different kind of complicated.

For your project specifically: the worm has no eyes and a behavior repertoire that's basically forward / reverse / turn, so a VLM has nothing to look at and nothing to decide. Stick with the fly. But if you want the worm for anything, it's as a calibration bench: it's the one nervous system where a validated, perturbation-fitted causal model exists. A 300-node linear dynamical system with measured ground truth is exactly the toy on which you'd test your telecom instruments (whitened matched filters, CRB efficiency, gain-control decomposition) before pointing them at flyvis or the VLM. That's the "step 8" prerequisite hiding in your question.

> can we already find out if the fly is thinking sugar or if the fly is doing some sort of planning and if it is planning, how it is planning?
> and if yes, might we be able to extract info out of the VLM once it controls the fly?

Three different questions hiding in there, with three different answers.

"Thinking sugar": yes, decodable, and mostly understood — depending on what you mean by thinking.

- Detecting sugar: solved. Sweet gustatory neurons (Gr5a/Gr64f) fire, the signal goes through the subesophageal zone to proboscis motor neurons, and the Shiu whole-brain model reproduced that entire pathway from connectivity alone. You can read "sugar on the tongue" off a handful of named neurons.
- Wanting sugar: partly. Hunger is an internal state carried by specific neuromodulatory populations (NPF, dopamine, insulin-like peptides) that change the gain of the sugar pathway. You can decode "hungry" from those cells, and you can flip it optogenetically.
- Remembering sugar, which is the closest to "thinking about sugar when it isn't there": also surprisingly good. The mushroom body pairs an odor code with a dopamine reward signal that sugar drives; later, the odor alone reactivates a valence readout in the output neurons. That's the fly representing sugar-value in sugar's absence, and it's readable from ~34 identifiable MBON types. Mushroom body learning is one of the two best-understood "cognitive" circuits in any animal.

Planning: not tree search, but explicit goals and vector arithmetic, and the mechanism is known at cell-type resolution. This is the other best-understood circuit, and you of all people will like it. The central complex compares internally generated estimates of the fly's heading and goal angles, both in world-centred coordinates, to produce a body-centred steering signal. The pieces: EPG "compass" neurons hold the heading as a ring attractor; FC2 cells carry the goal angle, and focal optogenetic activation of FC2 neurons makes flies orient along experimenter-defined directions; PFL3 cells receive both, show conjunctive tuning to heading and goal, and their output terminals carry the steering error. Then the part that's pure signal processing: three populations (PFL3R, PFL3L, PFL2) each receive a shifted copy of the head-direction vector, with reference frames offset roughly 120° from each other, and each compares its own copy against a common goal vector via a nonlinear transformation. The authors themselves call this copy-and-shift architecture reminiscent of a resolver servomotor. PFL3L drives left turns, PFL3R right turns, PFL2 increases rotational speed. Downstream, a 2025 connectome-modeling preprint argues for a layered control architecture in the lateral accessory lobes that arbitrates between goal pursuit and stimulus-driven overrides, with a dominant pro-goal push-pull circuit and a faster anti-goal circuit that inverts it.

So "how does the fly plan?": it stores a goal vector in the fan-shaped body, runs a compass, feeds both into a three-phase phase comparator that outputs a steering error, and layers an arbitration circuit on top for threats and detours. There's also evidence-accumulation-to-bound for olfactory decisions (Miesenböck's FoxP neurons) and path-integration vector memory in the same brain region. No evidence anywhere of the fly simulating futures. It's a servo with a settable setpoint, not a planner, and honestly that's a more interesting finding than "planning" would have been.

Extracting this from the VLM: yes, and this is a better mech-interp target than T4/T5. The fly gives you a full, cell-level spec of what a navigation controller "should" look like, so you can ask whether a VLM trained to do the same job converges on the same solution. Concretely:

- Design the task so a goal and a compass are required: menotaxis (hold an arbitrary heading relative to a landmark, survive perturbations that rotate the fly), or return-to-origin after a random walk. A pure "walk toward the visible thing" task needs neither.
- The VLM needs somewhere to keep state. A stateless single-frame model can't carry a heading. Give it a persistent context (previous-step summary tokens, or a scratchpad it writes and re-reads). This is a design decision, not a detail: it's where the ring attractor would have to live.
- Then the probes, in order of how impressed I'd be: (a) a linear probe for allocentric heading exists in the residual stream, and its topology is a ring (circular manifold, not a line) — trained RNNs are known to produce ring attractors for head direction, VLMs are not; (b) a separate goal representation exists, distinguishable from the heading one, and stays put while heading changes; (c) the emitted steering command is a function of heading–goal mismatch with the sinusoidal shape the PFL3 model predicts; (d) causal: rotate the heading probe direction and the fly turns the way a rotated EPG bump would make a real fly turn; rotate the goal direction and it re-targets, matching the FC2 optogenetics result. Matched intervention phenotypes again — that's the kill bar.
- For the sugar version: add a hunger token and food targets, probe for a value representation, and test whether "hunger" changes steering gain toward food the way it does in the fly, rather than changing the target.

One trap specific to VLMs: it may "plan" by literally emitting text ("target is left, turning left") before the action token. That's trivially decodable and tells you nothing about the computation unless you check faithfulness — ablate the text, see if behavior survives. If it does, the text was narration, and the real mechanism is where the probes are looking.

Bigger picture: if the VLM builds a compass and a goal vector and a phase comparator without being told to, you've shown that architecture is a convergent solution to embodied navigation, not a quirk of insect evolution. That's a result people outside both fields would care about.

- Path integration. The same brain region computes an allocentric travel-direction vector by literally adding phase-shifted sinusoids (PFN → hΔB neurons; Lyu, Abbott & Maimon 2022) and stores a vector memory of "where food was." It's vector arithmetic in a phasor representation, validated by imaging and perturbation. If the VLM has to return to a start point after a random walk, does it build phasor addition? Cleaner than steering because the math is more specific and there are fewer ways to fake it.
- Mushroom body associative learning. This is the one that connects to your field. Odors get a sparse random expansion (~2,000 Kenyon cells from ~50 glomeruli), a dopamine teaching signal that looks like a reward prediction error writes onto the KC→MBON synapses, and MBONs read out valence. Every piece is causally validated, and it's the same architecture as a sparse-expansion associative memory / one-shot in-context learning. If the fly must learn in-episode that odor A means sugar, the VLM has to do in-context association — and you get to ask whether it does it with an induction-head-like mechanism, a sparse expansion, or something else. That maps directly onto the induction-head literature you've already been ablating.
- Steering (EPG/FC2/PFL3), as discussed. Still excellent, but it's a servo. Which is exactly the problem with your 99.999% number.

> oh wait hold on
> this might be simple enough to do true training in real time
> that would be nuts

It is nuts, and it's the right instinct. Let me separate the versions, because one of them is the fly's actual trick and the others are traps.

What "real time" means here. The fly's learning is slow: one odor–sugar pairing produces a measurable memory over seconds, and the mushroom-body plasticity that stores it has time constants of seconds to minutes. Your control loop is 5 Hz. So "real time" is one weight update every few hundred milliseconds of sim time, and sim time isn't wall time — MuJoCo runs a fly at thousands of steps per second, and you can run a batch of flies in parallel, so a batch of flies is a batch of gradients. A LoRA step on a sub-1B model at 1 Hz is nothing. Compute is not the obstacle.

The obstacle is the teacher. Behavior cloning has a teacher. Online, you don't; you have sugar. That's the thing you have to build: a scalar reward signal, and that's not incidental — it's the dopamine neuron. In the fly, dopamine carries something close to a reward prediction error, broadcast to all KC→MBON synapses, and the synapse changes as a function of pre-activity × post-activity × dopamine, with sign depending on timing. Three-factor rule. And here's the part you'll like: that is Widrow–Hoff. LMS. A scalar error broadcast to every tap of an adaptive filter. The mushroom body is an adaptive equalizer trained on the sugar channel.

The three versions.

1. Backprop the whole VLM online with RL. Possible, and the one that will fail interestingly: catastrophic forgetting, reward hacking on sim exploits, and no biological analogue. The fly never does this. Keep it as a baseline.
2. Freeze the VLM, learn in-context. Give it the episode history in its context and let it "learn" without weight updates. This is what transformers naturally do, and it's the crucial contrast: the fly learns in weights, the LLM learns in activations. Which one a VLM prefers when it has both available is itself a question nobody has answered in an embodied setting.
3. The fly-faithful one, which I think is the actual project. Frozen VLM (or frozen flyvis) for features → a small plastic module with the mushroom body's architecture: sparse random expansion (say 2,000 units from a few hundred features, ~5% active), a linear readout to valence/steering gain, and a three-factor local rule gated by a reward-prediction-error signal you compute from sugar contact. No backprop through the module. It trains at physics rate, it can't forget the rest of the model because the rest is frozen, and every "synapse" is inspectable. The fly solved continual learning by making the plastic part tiny and modular; steal that.

Then the experiment is a Drosophila learning assay run in silico with a readable brain: single-pairing acquisition, extinction, reversal learning (MBs do reversal), and the killer one — does hunger state change gain on the learned readout rather than the readout itself, the way it does in the fly. Kill bars: learns within N pairings, reverses within M, generalizes to a novel odor at chance. All of this has real-fly numbers to compare against.

Where the interp gets sharp. Versions 2 and 3 give you two systems that solve the same one-shot association with different substrates. Compare them: does the VLM's in-context solution build something like a sparse expansion in activation space? Does its attention pattern for "odor A → sugar" look like an induction head doing associative recall, i.e. the same thing you were ablating in GPT-2? And the trap to pre-register: if you use LoRA for version 1, the update is low-rank by construction, so "we found a low-rank update on a sparse code" would be your tooling, not the model. Use full-parameter on a tiny head or don't make that claim.

The thing that actually excites me: a real fly's learning curve is observable from the outside and from a few dozen neurons at a time. Yours would be observable at every weight at every step, in a system whose reference architecture is known. You'd be watching the equalizer converge, tap by tap, with the biology telling you what the taps should look like.

---

## Build log, 2026-09-09 (Claude Code session in claude_box)

What exists after this session, all in `~/dev`:

- **`fly-brain`**: clone of philshiu/Drosophila_brain_model (Shiu et al. 2024)
  with the FlyWire v783 connectome (138,639 neurons, 15.1 M edges, 54.5 M
  synapses) shipped in-repo. Brian2 sugar experiment reproduced here:
  MN9 78.6 ± 4.6 Hz vs the paper's 80.4 ± 5.3 Hz.
- **`fly-brain-gpu`**: Rust + wgpu port of that LIF model. Four WGSL kernels
  per 0.1 ms step, 18-step delay ring, integer atomics for spike delivery,
  GPU Poisson input. MN9 77.7 ± 4.8 Hz, 435 responding neurons, r = 0.9997
  against Brian2 over all neurons. 30 trials in 3.2 s on the RTX 3090 Ti vs
  143 s for Brian2 on 10 processes. Stays inside baseline WebGPU limits;
  wasm build not yet attempted. Finding: Brian2's `unless refractory` also
  drops incoming synaptic and Poisson input during refractoriness.
- **NeuroMechFly v2** (`flygym` 1.2.1, Apache 2.0, MuJoCo 3.2.7) installed in
  the scratchpad venv. The v1 repo linked above is archived; v2 is the body.
- **This branch of `cairns-gfx`**: see `plans/gfx_fly-pose-override.md` in
  `~/dev/plans`. Per-actor joint pose override with no shader edits
  (`cairns.entity.setJointPose` / `clearJointPose` / `jointNames`),
  `tools/mjcf_to_rigged_glb.py` converting the flygym MJCF + STLs to
  `assets/fly.glb` (157 joints, bind pose matches MuJoCo FK to 4e-16), the
  `fly_pose` golden, a `linux-vk` platform key with 10 baked refs, Linux
  build fixes, and `scripts/run_vk_linux.sh`.
- **Container gotcha**: `Dockerfile.linux-vk` bakes `VK_ICD_FILENAMES` to
  lavapipe, hiding the 3090 Ti from every Vulkan client. See
  `~/dev/throwaway/hello_triangle/README.md` section 0.

Verified: golden suite on NVIDIA Vulkan, 46 cases / 19 pass / 27 skip / 0
fail. Unverified: Metal (cannot build here); lavapipe cannot boot the engine
because the 256 MB skin output buffer exceeds its 128 MB storage range.

Next: flygym's MuJoCo body driven by descending-neuron output from the brain
sim, streaming joint angles into `setJointPose`.
