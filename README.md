# alcor_ana_INFNGE

Analisi per ALCOR basata su ROOT/RDataFrame. Lavora **direttamente sui decoded** (alcdaq.fifo_*.root).

## Variabile ambiente
- Imposta `ALCOR_ANA_GE` al path di questa cartella per rendere i percorsi indipendenti dal nome:
  - `export ALCOR_ANA_GE=/Users/simone/Work/BNL/EIC/ePIC/ALCOR/directory_apcx12/alcor_ana_INFNGE`
- Gli script usano `ALCOR_ANA_GE` se definita.

## Struttura
- `script/` eseguibili principali
- `macro/` macro ROOT C++
- `config/` configurazioni (coppie di coincidenze)
- `calibration/` file di calibrazione prodotti
- `output/` PDF/ROOT/TXT/log prodotti
- `decoder/` decoder self‑contained (sorgenti/build/binari)
  - `decoder/src/` sorgenti
  - `decoder/build/` build dir (generato)
  - `decoder/local/` install dir (cmake install)
  - `decoder/bin/` binari finali usati dagli script

## Pipeline consigliata (decoded → output)
1) **Decode raw → decoded ROOT**
   - `script/decode_raw.sh /path/to/raw_or_run [--force]`
   - usa il decoder e scrive in `../data/<run>/kc705-196/decoded/alcdaq.fifo_*.root`
   - default decoder: `decoder/bin/decoder` (risolto via `ALCOR_ANA_GE`) (override con `DECODER_BIN=...`)

2) **Fine calibration (laser)**
   - `script/run_fine_calibration.sh -i ../data/calibration`
   - output: `calibration/fine_calibration.root` + `output/fine_calibration.pdf`
   - contiene: `hFineMin`, `hFineMax`, `hFineEntries`, `hFineTdc0..3`, **`hFineLut` (CDF)**

3) **Channel calibration (laser)**
   - `script/run_channel_calibration.sh -i ../data/calibration -k calibration/fine_calibration.root`
   - output: `calibration/channel_calibration.root`
   - curve ToT→correzione per canale, riferite al canale di riferimento (default 19)

4) **Coincidence (run di test)**
   - `script/run_coincidence.sh -i ../data/<run_test> -p config/coincidence_17_19.txt -k calibration/fine_calibration.root -K calibration/channel_calibration.root`
   - output: PDF + ROOT + TXT in `output/`
   - include: search/coinc window, FWHM, 2D Δt vs fine (search e coinc-only), profili

5) **(Opz.) Validazione LUT fine**
   - `script/run_fine_validation.sh -i ../data/calibration`
   - output: PDF + ROOT + TXT in `output/`

## Decoder (self‑contained)
- Sorgenti: `decoder/src/`
- Binari: `decoder/bin/decoder`
  - `decoder/bin/decoder` è un symlink al binario compilato in `decoder/build/src/decoder`.
- Build: `script/build_decoder.sh`
  - output principale: `decoder/bin/decoder`
  - usa dipendenze di sistema (ROOT/Boost; uHAL solo per readout tools)

## Script (script/)
- `decode_raw.sh` — raw `.dat` → decoded ROOT
- `build_decoder.sh` — build del decoder da `decoder/src/`
- `run_fine_calibration.sh` — calibrazione fine (min/max + LUT, PDF in `output/`)
- `run_fine_validation.sh` — validazione LUT fine (intrinseca + cross-validation)
- `run_channel_calibration.sh` — calibrazione ToT canale‑per‑canale
- `run_coincidence.sh` — analisi coincidenze, PDF/ROOT/TXT
- `run_plot.sh` — plot per canali (1D, spill)

## Macro (macro/)
- `analysis_io.h` — risoluzione input (decoded)
- `analysis_time.h` — timing, fine‑calib (lineare + LUT CDF), fine‑cut, calibrazione canale
- `fine_calibration_rdf.cxx` — costruisce `fine_calibration.root` (+ PDF opzionale)
- `channel_calibration_rdf.cxx` — costruisce `channel_calibration.root`
- `coincidence_rdf.cxx` — analisi coincidenze (PDF/ROOT/TXT, FWHM, 2D fine)
- `plot_channels_rdf.cxx` — plot 1D per canali/ spill
- `fine_validation_rdf.cxx` — validazione LUT fine (intrinseca + cross-validation)

## Note operative
- **Fine‑cut**
  - Se impostato, esclude hit con `|fine - cut| <= fine_cut` (cut = (min+max)/2 per TDC).
- **LUT fine (CDF)**
  - **TDC index**: indice globale per ogni TDC fisico, non solo 0–3.
    - Definizione (in `analysis_time.h`): `tdc_index = tdc + 4*pixel + 16*(column%2) + 32*fifo`.
    - Razionale: per ogni FIFO ci sono 32 TDC fisici (2 parità di colonna × 4 pixel × 4 TDC).
    - Serve a costruire LUT/linearizzazione per **ogni TDC fisico**, senza mescolare risposte diverse.
  - `fine_calibration_rdf` costruisce `hFineLut` con la CDF per ogni TDC index.
  - Per ogni bin di fine `b` con conteggio `c_b` e totale `N`:
    ```text
    cum_b = Σ_{k<=b} c_k
    frac_b = clamp((cum_b - 0.5*c_b) / N, 0, 1)
    fine_fraction = frac_b - 0.5
    ```
  - Il tempo è calcolato come: `time_ns = (time_tick - fine_fraction) * tick_ns`.
  - Se `hFineLut` non è presente, si usa la mappatura lineare min/max con wrap al cut.
  - Per forzare il **no‑LUT** (solo min/max) anche se `hFineLut` esiste: `--no-lut`
  - **Time‑walk (ToT)**: corretto dalla calibrazione canale‑per‑canale (curve ToT→Δt).
    - Rilevante quando la ToT varia; tende a restringere il picco di coincidenza.
    - Disattivabile con `--no-chan-calib` in `run_coincidence.sh`.
  - La calibrazione di canale è **simmetrica di default** (correzione divisa tra ref e canale).
    - Disabilita con `run_channel_calibration.sh --no-symmetrize-ref`.
    - In modalità simmetrica, il riferimento è la **media** delle correzioni dei canali coinvolti.
  - Viene stimato anche un **offset assoluto per canale** dal run di calibrazione:
    - per ogni canale si fa un fit gaussiano del residuo Δt dopo la correzione ToT
    - l’offset è poi applicato in analisi (split 50/50 in modalità simmetrica)

## Aggiornamenti recenti (calibrazione 2 canali)
- **Calibrazione simmetrica** per il canale di riferimento:
  - `run_channel_calibration.sh --symmetrize-ref` riempie anche la correzione del ref (utile con soli 2 canali).
  - Con più canali, la correzione del ref può “mischiare” contributi (warning in macro).
  - I plot 2D Δt vs fine (search/coinc‑only) sono **zoomati** sull’asse fine in [20, 160] per chiarezza.
  - Nei confronti ROOT, sui 2D sono riportati anche:
    - **corr(Δt, fine)** (correlazione lineare)
    - **slope** e **span** del profilo ⟨Δt⟩(fine)

- **Metriche di valutazione LUT (fine_validation)**
  - **KS D**: distanza di Kolmogorov‑Smirnov tra CDF empirica di `fine_fraction` e uniforme in \([-0.5, 0.5]\).
    - **Più piccolo è meglio** (0 = uniformità perfetta).
  - **p‑value KS**: probabilità di osservare una D almeno così grande se la distribuzione è uniforme.
    - **Più alto è meglio** (p ≪ 0.05 indica non uniformità significativa).
  - **mean**: media di `fine_fraction` (idealmente ~0).
  - **std**: deviazione standard di `fine_fraction` (idealmente ~0.288675 per uniforme in \([-0.5,0.5]\)).
  - Le metriche sono riportate per **tdc0..tdc3** e **all**, sia per **intrinsic** che **cross**.
- **Config coincidences** (`config/*.txt`)
  - Riga coppia: `chA chB [window_ns]`
  - Riga gruppo: `group ch1 ch2 ch3 [window=ns]`

## run_coincidence defaults
- `window_ns=10`, `clock_mhz=320`, `use_fine=1`, `use_lut=1`
- `duration_ns=15` (<=0 disabilita filtro durata), `fine_cut=0` (disabilitato)
- `calib`: `calibration/fine_calibration.root`
- `chan-calib`: `calibration/channel_calibration.root`
- output: PDF + ROOT + TXT (nomi derivati dall’input)
