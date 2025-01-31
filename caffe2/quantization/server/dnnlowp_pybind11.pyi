# pyre-strict
from typing import Tuple, List, Dict

class QueryTensorQparam:
    def __init__(self, scale: float, zero_point: int, precision: int) -> None: ...
    def scale(self) -> float: ...
    def zero_point(self) -> int: ...
    def precision(self) -> int: ...
    def min(self) -> float: ...
    def max(self) -> float: ...

class HistogramNetObserver:
    pass

class OutputColumnMaxHistogramNetObserver:
    pass

class RegisterQuantizationParamsWithHistogramNetObserver:
    pass

def ClearNetObservers() -> None: ...
def ObserveMinMaxOfOutput(min_max_file_name: str, dump_freq: int = -1, delimiter: str = " ") -> None: ...
def ObserveHistogramOfOutput(out_file_name: str, dump_freq: int = -1, mul_nets: bool = False, op_filter: str = "", delimiter: str = " ") -> None: ...
def DumpHistogramFile(ob: HistogramNetObserver) -> None: ...
def AddHistogramObserver(net_name: str, out_file_name: str, dump_freq: int = -1, mul_nets: bool = False, delimiter: str = " ") -> HistogramNetObserver: ...
def DumpOutputColumnMaxHistogramFile(ob: OutputColumnMaxHistogramNetObserver) -> None: ...
def AddOutputColumnMaxHistogramObserver(net_name: str, out_file_name: str, observe_column_max_for_blobs: List[str], dump_freq: int = -1, bin_nums: int = 16, mul_nets: bool = False, delimiter: str = " ") -> OutputColumnMaxHistogramNetObserver: ...
def ChooseQuantizationParams(blob_name: str) -> Tuple[float, int]: ...
def RegisterQuantizationParams(min_max_file_name: str, is_weight: bool = False, qparams_output_file_name: str = "") -> None: ...
def RegisterQuantizationParamsWithHistogram(histogram_file_name: str, is_weight: bool = False, qparams_output_file_name: str = "") -> None: ...
def AddRegisterQuantizationParamsWithHistogramObserver(net_name: str, histogram_file_name: str, is_weight: bool = False, qparams_output_file_name: str = "") -> RegisterQuantizationParamsWithHistogramNetObserver: ...
def AddScaleZeroOffsetArgumentsWithHistogram(net_def_bytes: bytes, histogram_file_name: str) -> bytes: ...
def get_fakefp16_mapping(use_fp16_acc: bool, use_nnpi: bool) -> Dict[str, str]: ...
def freeze_quantization_params(net_def_bytes: bytes) -> bytes: ...
def ChooseStaticQuantizationParams(min: float, max: float, bins: List[int], preserve_sparsity: bool = True, precision: int = 8, quant_scheme: str = "min_max", p99_threshold: float = 0.99, is_weight: bool = False) -> QueryTensorQparam: ...
def ObserveFp16FCPackedWeights(blob_name: str, weights_out_file: str) -> None: ...
def ObserveInt8FCPackedWeights(blob_name: str, weights_out_file: str) -> None: ...
def CreateInt8QuantSchemeBlob(quant_scheme_blob_name: str, quantization_kind: str, preserve_sparsity: bool) -> None: ...
def CreateInt8QuantParamsBlob(quant_param_blob_name: str, scale: float, zero_point: int) -> None: ...
def ObserveInt8QuantParamsBlob(quant_params_blob_name: str) -> Tuple[float, int]: ...
def ObserveInt8QuantSchemeBlob(quant_scheme_blob_name: str) -> Tuple[str, bool]: ...
