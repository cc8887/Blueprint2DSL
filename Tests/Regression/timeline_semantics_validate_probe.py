import json
import os
import traceback

import unreal


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULT_PATH = os.path.join(SCRIPT_DIR, "timeline_semantics_validate_probe_result.json")

TIMELINE_DSL = r'''(timeline
  "Timeline_0"
  :length 2
  :length-mode timeline-length
  :autoplay false
  :loop true
  :replicated true
  :ignore-time-dilation true
  :metadata ("Category" "Timeline Regression")
  :track
    (event
      "Pulse"
      :external false
      :expanded true
      :curve-view-synchronized true
      :curve
        (rich-curve
          :pre-extrap constant
          :post-extrap constant
          :key (key :time 0.5 :value 0 :interp constant :tangent none :weight none)))
  :track
    (float
      "Alpha"
      :external false
      :curve
        (rich-curve
          :default 0
          :pre-extrap cycle
          :post-extrap cycle-with-offset
          :key
            (key
              :time 0
              :value 0
              :interp cubic
              :tangent user
              :weight both
              :arrive 0
              :leave 1
              :arrive-weight 0.25
              :leave-weight 0.5)
          :key (key :time 2 :value 1 :interp linear :tangent auto :weight none)))
  :track
    (vector
      "Offset"
      :external false
      :curve
        (vector-curve
          :x (rich-curve :key (key :time 0 :value 0))
          :y (rich-curve :key (key :time 0 :value 1))
          :z (rich-curve :key (key :time 0 :value 2))))
  :track
    (linear-color
      "Tint"
      :external false
      :curve
        (linear-color-curve
          :r (rich-curve :key (key :time 0 :value 1))
          :g (rich-curve :key (key :time 0 :value 0))
          :b (rich-curve :key (key :time 0 :value 0))
          :a (rich-curve :key (key :time 0 :value 1))
          :adjust-hue 0
          :adjust-saturation 1
          :adjust-brightness 1
          :adjust-brightness-curve 1
          :adjust-vibrance 0
          :adjust-min-alpha 0
          :adjust-max-alpha 1))
  :update
    (PrintString
      :instring "Timeline update"
      :duration (timeline-output :timeline "Timeline_0" :out-pin "Alpha" :id "11223344"))
  :finished (PrintString :instring "Timeline finished")
  :event ("Pulse" (PrintString :instring "Timeline pulse"))
  :pos "320,160"
  :id "11223344")

(event ControlPlay
  (timeline-control :timeline "Timeline_0" :action play))
(event ControlPlayFromStart
  (timeline-control :timeline "Timeline_0" :action play-from-start))
(event ControlStop
  (timeline-control :timeline "Timeline_0" :action stop))
(event ControlReverse
  (timeline-control :timeline "Timeline_0" :action reverse))
(event ControlReverseFromEnd
  (timeline-control :timeline "Timeline_0" :action reverse-from-end))
(event ControlSetNewTime
  (timeline-control :timeline "Timeline_0" :action set-new-time :time 0.75))'''

INVALID_PARSE_DSL = '(timeline "Broken" :length 1'
INVALID_TOP_LEVEL_DSL = '(timeline-widget "NotATimeline")'

report = {
    "success": False,
    "probe": "timeline_semantics_validate",
    "asset_independent": True,
    "cases": [],
    "errors": [],
}


def get_bool(result, *names):
    for name in names:
        if hasattr(result, name):
            return bool(getattr(result, name))
    return False


def get_text(result, *names):
    for name in names:
        if hasattr(result, name):
            value = getattr(result, name)
            if value is not None:
                return str(value)
    return ""


def validate_case(case_id, dsl_text, expected_success):
    result = unreal.BlueprintLispPythonBridge.validate_dsl(dsl_text)
    actual_success = get_bool(result, "success", "b_success")
    entry = {
        "id": case_id,
        "expected_success": expected_success,
        "actual_success": actual_success,
        "message": get_text(result, "message"),
    }
    report["cases"].append(entry)
    if actual_success != expected_success:
        raise RuntimeError(
            f"{case_id}: expected success={expected_success}, actual={actual_success}, "
            f"message={entry['message']}"
        )


try:
    validate_case("timeline_all_internal_tracks_and_controls", TIMELINE_DSL, True)
    validate_case("timeline_unclosed_form", INVALID_PARSE_DSL, False)
    validate_case("timeline_unknown_top_level_form", INVALID_TOP_LEVEL_DSL, False)
    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
    report["errors"].append(traceback.format_exc())
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as output_file:
        json.dump(report, output_file, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
