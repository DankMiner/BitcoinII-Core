# Work maturity baseline

The proposed baseline is the hardest sustained epoch found in the chain study,
with maturity centered at **8,580 blocks**. The minimum remains **4,200 blocks**
and the maximum remains **12,960 blocks**.

Two complete 2,016-block epochs share that difficulty:

- [48,384–50,399](https://www.bc2.live/api/v1/blocks/48384)
- [52,416–54,431](https://www.bc2.live/api/v1/blocks/52416)

Both use `nBits = 0x181cebcd`, corresponding to a difficulty of approximately
**38,017,052,330.22743**. This uses a sustained epoch as the reference; the
single-block difficulty record is not the baseline.

## Exact work requirement

Decode the reference compact target as:

```text
T = 709143230170313076936825019504629630928466900935180812288
reference block work = floor(2^256 / (T + 1))
                     = 163284487972206562362

center age = (4200 + 12960) / 2 = 8580
required work W = 8579 × reference block work
                = 1400817622313560098503598

W as a 256-bit hexadecimal integer:
0000000000000000000000000000000000000000000128a279b84c5571b87bae
```

The multiplier is **8,579** because the reward block and the prospective spending
block do not count. A reward spent at age 8,580 has 8,579 completed blocks between
its creation and the spending block. If all of those blocks have the reference
target, their accumulated work reaches the requirement exactly.

The requirement is a **fixed constant**. It does not move when a new difficulty
record is set, and it does not use a rolling 500-block average. The recent
500-block average below is only a comparison with observed network conditions.

## Expected behavior

For a steady amount of work per following block, the modeled maturity age is:

```text
max(4200, min(12960, 1 + ceil(W / work_per_block)))
```

| Following blocks' average work | Approximate equivalent difficulty | Maturity age |
| --- | ---: | ---: |
| Half the reference | 19.009 billion | 12,960 |
| Reference | 38.017 billion | 8,580 |
| Twice the reference | 76.034 billion | 4,291 |
| Three times the reference | 114.051 billion | 4,200 |
| Latest 500-block average in the study | 3.340 billion | 12,960 |

These are steady-work examples, not payout forecasts. Actual maturity uses the
sum of the work of the blocks that follow each reward. Block age also does not
guarantee a particular number of days.

## What the historical comparison shows

The study snapshot ends at block **59,819**, retrieved on **September 23, 2026**.
Total reconstructed work from genesis through that block is
`1208279390541217024284079`. The proposed requirement is **15.9349% greater than
that entire total**. No reward in the observed history could therefore collect
enough following work to reach this requirement. Every historical reward with a
complete 12,960-block observation period would mature at the maximum age under
this rule.

The latest 500 blocks in that snapshot have total work
`7173126543105838209767`. Maintaining that average would require an age of about
97,645 blocks to reach the work requirement alone; the 12,960-block cap would
release the reward first. Centering on the hardest epoch deliberately sets a
high bar relative to the chain's observed average work.

The history came from one explorer. Legacy work was reconstructed from exact
epoch targets, and the study checked sampled headers and the available
post-ShockWave headers. It was not independently verified with a fully
synchronized node. Only 2,070 post-ShockWave blocks were present, so the snapshot
does not contain a complete 4,200-block maturity window under those rules.

## Activation

This calibration is the proposed mainnet work target. **Mainnet activation
remains disabled** in the prototype. Selecting the work target does not select
an activation height or change the running network.

The proposal still needs developer and community review, published tested code,
and the required discussion period before an activation height is set. The
baseline should be checked against independently synchronized chain data during
that review.
