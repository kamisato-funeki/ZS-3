"""由独立KLayout批处理执行；-rd gds_path=... -rd output_path=... 提供路径。"""
import json
import math
from pathlib import Path
import pya

# KLayout将-rd变量注入脚本全局；没有从被测C++ writer复用任何二进制解码逻辑。
layout = pya.Layout()  # 独立KLayout数据库。
layout.read(gds_path)
cells = list(layout.top_cells())  # 输入的顶层结构集合。
assert len(cells) == 1, "必须恰好一个顶层结构"
top = cells[0]  # 当前唯一顶层。
metadata = json.loads(Path(gds_path + ".metadata.json").read_text())  # 伴随单位/图层契约。
assert abs(layout.dbu - metadata["dbu_um"]) < 1e-15, "GDS UNITS不同于声明DBU"
layers = {}  # 独立回读每层元素数量。
boxes = {}  # 实体层BOUNDARY物理范围，单位为整数DBU。
for layer_index in layout.layer_indexes():  # KLayout内部图层索引。
    info = layout.get_info(layer_index)  # 文件layer/datatype标识。
    shapes = list(top.shapes(layer_index).each())  # 该层真实元素。
    layers[str(info.layer)] = len(shapes)
    boxes[str(info.layer)] = [[shape.bbox().left, shape.bbox().bottom, shape.bbox().right, shape.bbox().top] for shape in shapes]
assert layers == metadata["layers"], (layers, metadata["layers"])
# tiny夹具是独立手算判定器：两个3×2 DU标准单元，初始左下角(1,0)/(2,0)。
if "source_path" not in globals():
    assert sorted(boxes["1"]) == [[1000, 0, 4000, 2000], [2000, 0, 5000, 2000]], boxes["1"]
else:
    source = json.loads(Path(source_path).read_text())  # 带master尺寸和固定性的源快照。
    assert source["datasetId"] == metadata["datasetId"]
    assert source["runId"] == metadata["runId"] and source["revision"] == metadata["revision"]
    expected = {"1": [], "2": [], "3": []}  # 独立计算实体层BOUNDARY。
    def quantize(value):
        """物理μm坐标以最近整数、中点远离零量化，返回DBU。"""
        return int(math.copysign(math.floor(abs(value) / layout.dbu + 0.5), value))
    for node in source["nodes"]:  # 源真实实例，无filler。
        width = node["width"]  # N朝向宽DU。
        height = node["height"]  # N朝向高DU。
        if node["orientation"] in ["W", "E", "FW", "FE"]:
            width, height = height, width
        if width * height == 0:
            continue
        layer = "3" if node["fixed"] else "2" if node["kind"] == "macro" else "1"  # 互斥实体层。
        scale = metadata["microns_per_du"]  # 本次明确导出物理/显示比例。
        expected[layer].append([quantize(node["llx"] * scale), quantize(node["lly"] * scale), quantize((node["llx"] + width) * scale), quantize((node["lly"] + height) * scale)])
    for obstacle in source.get("obstacles", []):  # 单独声明的硬阻挡。
        expected["3"].append([quantize(value * metadata["microns_per_du"]) for value in obstacle])
    for layer in ["1", "2", "3"]:  # 端口PATH仅在数量上核对，不当作有面积实例。
        actual = []  # 当前实体层回读的BOUNDARY。
        for layer_index in layout.layer_indexes():
            if str(layout.get_info(layer_index).layer) != layer:
                continue
            for shape in top.shapes(layer_index).each():
                if shape.is_path():
                    continue
                box = shape.bbox()  # 独立KLayout几何包围盒。
                actual.append([box.left, box.bottom, box.right, box.top])
        assert sorted(actual) == sorted(expected[layer]), (layer, actual, expected[layer])
result = {"schemaVersion": 1, "reader": "KLayout", "reader_version": pya.Application.instance().version(), "dbu_um": layout.dbu, "top_cell": top.name, "layers": layers, "boxes_dbu": boxes}  # 独立回读证据。
Path(output_path).write_text(json.dumps(result, indent=2))
print("Independent KLayout geometry verification passed")
