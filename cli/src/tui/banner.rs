//! Banner ASCII e tagline della Pulse CLI, incorporati come costanti nel
//! sorgente.
//!
//! L'arte è EMBED direttamente qui (non letta da `public/`, che è gitignored)
//! così che il binario sia self-contained sia nella TUI full-screen sia nel
//! fallback testuale non-TTY.

/// Arte ASCII "PULSE CLI" renderizzata in verde Pulse nella TUI.
pub const PULSE_BANNER: &str = r"██████╗ ██╗   ██╗██╗     ███████╗███████╗     ██████╗██╗     ██╗
██╔══██╗██║   ██║██║     ██╔════╝██╔════╝    ██╔════╝██║     ██║
██████╔╝██║   ██║██║     ███████╗█████╗      ██║     ██║     ██║
██╔═══╝ ██║   ██║██║     ╚════██║██╔══╝      ██║     ██║     ██║
██║     ╚██████╔╝███████╗███████║███████╗    ╚██████╗███████╗██║
╚═╝      ╚═════╝ ╚══════╝╚══════╝╚══════╝     ╚═════╝╚══════╝╚═╝";

/// Tagline mostrata sotto il banner (voce del README pubblico).
pub const PULSE_TAGLINE: &str =
    "Restarting from the beat — built for devs, made for players, here for everyone.";

// ===========================================================================
// Animazione "battito": onda di luce che attraversa il banner (Req: TUI heart-
// beat). La matematica del colore è una funzione PURA di (riga, colonna,
// larghezza, tempo trascorso) — nessuna dipendenza da ratatui/crossterm — così
// da poterla ragionare e testare in isolamento. `mod.rs` la converte in
// `Color::Rgb` per riga/carattere del banner.
// ===========================================================================

use std::time::Duration;

/// Stato "a riposo": verde Pulse profondo (fondo della banda, lontano dall'onda).
pub const PULSE_REST: (u8, u8, u8) = (0, 120, 70);
/// Verde Pulse brillante (metà banda).
pub const PULSE_BRIGHT: (u8, u8, u8) = (0, 230, 118);
/// Cresta menta/quasi-bianca (picco dell'onda).
pub const PULSE_CREST: (u8, u8, u8) = (170, 255, 225);

/// Durata di uno sweep completo del battito (secondi). ~1.8s = ritmo cardiaco
/// calmo, non frenetico (accessibilità: nessun lampeggio brusco).
pub const PULSE_PERIOD_SECS: f32 = 1.8;

/// Leggera inclinazione della fronte d'onda per riga (in colonne): fa leggere
/// l'onda come una "scanline" appena diagonale che scorre nel logo.
const ROW_SKEW_COLS: f32 = 1.2;

/// Intensità della banda luminosa in `[0, 1]` per un carattere, calcolata come
/// falloff gaussiano rispetto alla distanza dalla colonna corrente dell'onda.
///
/// - Al centro dell'onda (`d == 0`) l'intensità è `1.0` (cresta).
/// - Lontano dall'onda tende a `0.0` (riposo).
/// - L'onda entra da sinistra ed esce completamente a destra ogni
///   [`PULSE_PERIOD_SECS`] secondi, con un margine pari a `3σ` così che la
///   banda sia interamente fuori schermo agli estremi del ciclo.
pub fn band_intensity(row: usize, col: usize, width: usize, elapsed: Duration) -> f32 {
    let width_f = width.max(1) as f32;
    // Fase del ciclo in [0, 1) dal tempo di parete (liscia, non legata ai frame).
    let phase = elapsed.as_secs_f32().rem_euclid(PULSE_PERIOD_SECS) / PULSE_PERIOD_SECS;
    // Ampiezza della banda: ~16% della larghezza, con un minimo per banner corti.
    let sigma = (width_f * 0.16).max(4.0);
    // Margine che consente all'onda di entrare/uscire completamente dallo schermo.
    let margin = sigma * 3.0;
    // Colonna corrente della fronte d'onda: da -margin a width+margin.
    let wave = -margin + phase * (width_f + 2.0 * margin);
    // Leggero skew diagonale riga per riga.
    let center = wave + row as f32 * ROW_SKEW_COLS;
    let d = col as f32 - center;
    (-(d * d) / (2.0 * sigma * sigma)).exp()
}

/// Interpolazione lineare fra due canali `u8`, con `t` clampato in `[0, 1]` e
/// risultato clampato in `[0, 255]`.
fn lerp_u8(a: u8, b: u8, t: f32) -> u8 {
    let t = t.clamp(0.0, 1.0);
    let v = a as f32 + (b as f32 - a as f32) * t;
    v.round().clamp(0.0, 255.0) as u8
}

/// Interpolazione lineare fra due colori RGB.
fn lerp_rgb(a: (u8, u8, u8), b: (u8, u8, u8), t: f32) -> (u8, u8, u8) {
    (lerp_u8(a.0, b.0, t), lerp_u8(a.1, b.1, t), lerp_u8(a.2, b.2, t))
}

/// Colore RGB di un singolo carattere del banner al tempo `elapsed`.
///
/// Mappa l'intensità della banda su una rampa a tre fermate:
/// - `0.0` → [`PULSE_REST`]  (verde profondo a riposo),
/// - `0.5` → [`PULSE_BRIGHT`] (verde Pulse brillante),
/// - `1.0` → [`PULSE_CREST`]  (cresta menta quasi-bianca).
///
/// I canali risultanti restano sempre entro l'inviluppo delle tre fermate
/// (quindi validi in `0..=255`).
pub fn pulse_char_rgb(row: usize, col: usize, width: usize, elapsed: Duration) -> (u8, u8, u8) {
    let i = band_intensity(row, col, width, elapsed);
    if i <= 0.5 {
        lerp_rgb(PULSE_REST, PULSE_BRIGHT, i / 0.5)
    } else {
        lerp_rgb(PULSE_BRIGHT, PULSE_CREST, (i - 0.5) / 0.5)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    /// A metà ciclo (phase = 0.5) la fronte d'onda è esattamente al centro del
    /// banner: l'intensità lì è massima (~1.0) e cala allontanandosi.
    #[test]
    fn intensity_peaks_at_wave_and_dims_far_away() {
        let width = 60;
        let mid = Duration::from_millis((PULSE_PERIOD_SECS * 500.0) as u64); // period/2

        let peak = band_intensity(0, 30, width, mid); // colonna centrale
        let left = band_intensity(0, 0, width, mid);
        let right = band_intensity(0, width, width, mid);

        assert!((peak - 1.0).abs() < 1e-3, "peak intensity should be ~1.0, got {peak}");
        assert!(left < 0.05, "far-left intensity should be near 0, got {left}");
        assert!(right < 0.05, "far-right intensity should be near 0, got {right}");
        assert!(peak > left && peak > right, "peak must dominate the edges");
    }

    /// Al picco dell'onda il colore è esattamente la cresta menta.
    #[test]
    fn color_at_peak_is_crest() {
        let width = 60;
        let mid = Duration::from_millis((PULSE_PERIOD_SECS * 500.0) as u64);
        assert_eq!(pulse_char_rgb(0, 30, width, mid), PULSE_CREST);
    }

    /// Lontano dall'onda il colore torna praticamente al verde di riposo.
    #[test]
    fn color_far_from_wave_is_near_rest() {
        let width = 60;
        let mid = Duration::from_millis((PULSE_PERIOD_SECS * 500.0) as u64);
        let (r, g, b) = pulse_char_rgb(0, 0, width, mid);
        // Vicino a REST (0,120,70); tolleranza per il residuo gaussiano.
        assert!(r <= 8, "r should be near rest 0, got {r}");
        assert!((g as i32 - PULSE_REST.1 as i32).abs() <= 12, "g should be near rest, got {g}");
        assert!((b as i32 - PULSE_REST.2 as i32).abs() <= 12, "b should be near rest, got {b}");
    }

    /// Proprietà: per qualunque (riga, colonna, tempo) ogni canale resta entro
    /// l'inviluppo delle tre fermate della palette (dunque valido in 0..=255).
    #[test]
    fn channels_stay_within_palette_envelope() {
        let width = 63;
        for step in 0..200u64 {
            let elapsed = Duration::from_millis(step * 25); // copre più cicli
            for row in 0..6usize {
                for col in 0..=width {
                    let (r, g, b) = pulse_char_rgb(row, col, width, elapsed);
                    // r: min 0 (rest/bright) .. max 170 (crest)
                    assert!(r <= PULSE_CREST.0, "r out of envelope: {r}");
                    // g: min 120 (rest) .. max 255 (crest)
                    assert!(
                        g >= PULSE_REST.1 && g <= PULSE_CREST.1,
                        "g out of envelope: {g}"
                    );
                    // b: min 70 (rest) .. max 225 (crest)
                    assert!(
                        b >= PULSE_REST.2 && b <= PULSE_CREST.2,
                        "b out of envelope: {b}"
                    );
                }
            }
        }
    }

    /// L'intensità è monotona decrescente allontanandosi dalla fronte d'onda.
    #[test]
    fn intensity_is_monotonic_around_the_wave() {
        let width = 60;
        let mid = Duration::from_millis((PULSE_PERIOD_SECS * 500.0) as u64);
        // Centro a col 30: intensità deve calare man mano che ci si allontana.
        let mut prev = band_intensity(0, 30, width, mid);
        for col in 31..=45 {
            let cur = band_intensity(0, col, width, mid);
            assert!(cur <= prev, "intensity should not increase moving away (col {col})");
            prev = cur;
        }
    }
}
