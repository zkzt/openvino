// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <ngraph/variant.hpp>
#include "ngraph/ops.hpp"
#include "ngraph/opsets/opset.hpp"
#include "pugixml.hpp"
#include "transformations/serialize.hpp"

using namespace ngraph;

NGRAPH_RTTI_DEFINITION(ngraph::pass::Serialize, "Serialize", 0);

namespace {  // helpers
template <typename Container>
std::string join(const Container& c, const char* glue = ", ") {
    std::stringstream oss;
    const char* s = "";
    for (const auto& v : c) {
        oss << s << v;
        s = glue;
    }
    return oss.str();
}

struct Edge {
    int from_layer = 0;
    int from_port = 0;
    int to_layer = 0;
    int to_port = 0;
};

// Here operation type names are translated from ngraph convention to IR
// convention. Most of them are the same, but there are exceptions, e.g
// Constant (ngraph name) and Const (IR name). If there will be more
// discrepancies discoverd, translations needs to be added here.
const std::unordered_map<std::string, std::string> translate_type_name_translator = {
    {"Constant", "Const"},
    {"Relu", "ReLU"},
    {"Softmax", "SoftMax"}};

std::string translate_type_name(const std::string& name) {
    auto found = translate_type_name_translator.find(name);
    if (found != end(translate_type_name_translator)) {
        return found->second;
    }
    return name;
}

class XmlSerializer : public ngraph::AttributeVisitor {
    pugi::xml_node& m_xml_node;
    std::ostream& m_bin_data;
    std::string& m_node_type_name;

    template <typename T>
    std::string create_atribute_list(
        ngraph::ValueAccessor<std::vector<T>>& adapter) {
        return join(adapter.get());
    }

public:
    XmlSerializer(pugi::xml_node& data,
                  std::ostream& bin_data,
                  std::string& node_type_name)
        : m_xml_node(data)
        , m_bin_data(bin_data)
        , m_node_type_name(node_type_name) {
    }

    void on_adapter(const std::string& name,
                    ngraph::ValueAccessor<void>& adapter) override {
        (void)name;
        (void)adapter;
    }

    void on_adapter(const std::string& name,
                    ngraph::ValueAccessor<void*>& adapter) override {
        if (name == "value" &&  translate_type_name(m_node_type_name) == "Const") {
            using AlignedBufferAdapter =
                ngraph::AttributeAdapter<std::shared_ptr<runtime::AlignedBuffer>>;
            if (auto a = ngraph::as_type<AlignedBufferAdapter>(&adapter)) {
                const int64_t size = a->size();
                const int64_t offset = m_bin_data.tellp();

                m_xml_node.append_attribute("offset").set_value(offset);
                m_xml_node.append_attribute("size").set_value(size);

                auto data = static_cast<const char*>(a->get_ptr());
                m_bin_data.write(data, size);
            }
        }
    }

    void on_adapter(const std::string& name,
                    ngraph::ValueAccessor<bool>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(adapter.get());
    }
    void on_adapter(const std::string& name,
                    ngraph::ValueAccessor<std::string>& adapter) override {
        if ((m_node_type_name == "GenericIE") &&
            (name == "__generic_ie_type__")) {
            // __generic_ie_type__  in GenericIE should not be serialized as a
            // <data> since it's purpose is to hold name of the layer type
            // it is a WA to not introduce dependency on plugin_api library
            m_node_type_name = adapter.get();
        } else {
            m_xml_node.append_attribute(name.c_str())
                .set_value(adapter.get().c_str());
        }
    }
    void on_adapter(const std::string& name,
                    ngraph::ValueAccessor<int64_t>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(adapter.get());
    }
    void on_adapter(const std::string& name,
                    ngraph::ValueAccessor<double>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(adapter.get());
    }
    void on_adapter(
        const std::string& name,
        ngraph::ValueAccessor<std::vector<int64_t>>& adapter) override {
        m_xml_node.append_attribute(name.c_str())
            .set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(
        const std::string& name,
        ngraph::ValueAccessor<std::vector<uint64_t>>& adapter) override {
        m_xml_node.append_attribute(name.c_str())
            .set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(
        const std::string& name,
        ngraph::ValueAccessor<std::vector<float>>& adapter) override {
        m_xml_node.append_attribute(name.c_str())
            .set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(
        const std::string& name,
        ngraph::ValueAccessor<std::vector<std::string>>& adapter) override {
        m_xml_node.append_attribute(name.c_str())
            .set_value(create_atribute_list(adapter).c_str());
    }
};

void visit_exec_graph_node(pugi::xml_node& data, std::string& node_type_name,
                           const ngraph::Node* n) {
    for (const auto& param : n->get_rt_info()) {
        if (auto variant =
                std::dynamic_pointer_cast<ngraph::VariantImpl<std::string>>(param.second)) {
            std::string name = param.first;
            std::string value = variant->get();

            if (name == "layerType") {
                node_type_name = value;
            } else {
                data.append_attribute(name.c_str()).set_value(value.c_str());
            }
        }
    }
}

const std::unordered_map<ngraph::Node*, int> create_layer_ids(
    const ngraph::Function& f) {
    std::unordered_map<ngraph::Node*, int> layer_ids;
    int id = 0;
    for (const auto& node : f.get_ordered_ops()) {
        layer_ids[node.get()] = id++;
    }
    return layer_ids;
}

const std::vector<Edge> create_edge_mapping(
    const std::unordered_map<ngraph::Node*, int>& layer_ids,
    const ngraph::Function& f) {
    std::vector<Edge> edges;
    for (const auto& node : f.get_ordered_ops()) {
        if (ngraph::op::is_parameter(node)) {
            continue;
        }

        for (const auto& i : node->inputs()) {
            auto source_output = i.get_source_output();
            auto source_node = source_output.get_node();
            auto current_node = i.get_node();

            NGRAPH_CHECK(layer_ids.find(source_node) != layer_ids.end(),
                         "Internal error");
            NGRAPH_CHECK(layer_ids.find(current_node) != layer_ids.end(),
                         "Internal error");

            Edge e{};
            e.from_layer = layer_ids.find(source_node)->second;
            e.from_port =
                source_node->get_input_size() + source_output.get_index();
            e.to_layer = layer_ids.find(current_node)->second;
            e.to_port = i.get_index();
            edges.push_back(e);
        }
    }
    std::sort(begin(edges), end(edges),
              [](const Edge& a, const Edge& b) -> bool {
                  return a.from_layer < b.from_layer;
              });
    return edges;
}



std::string get_opset_name(
    const ngraph::Node* n,
    const std::map<std::string, ngraph::OpSet>& custom_opsets) {
    auto opsets = std::array<std::reference_wrapper<const ngraph::OpSet>, 5>{
        ngraph::get_opset1(), ngraph::get_opset2(), ngraph::get_opset3(),
        ngraph::get_opset4(), ngraph::get_opset5()};

    // return the oldest opset name where node type is present
    for (int idx = 0; idx < opsets.size(); idx++) {
        if (opsets[idx].get().contains_op_type(n)) {
            return "opset" + std::to_string(idx + 1);
        }
    }

    for (const auto& custom_opset : custom_opsets) {
        std::string name = custom_opset.first;
        ngraph::OpSet opset = custom_opset.second;
        if (opset.contains_op_type(n)) {
            return name;
        }
    }

    return "experimental";
}


std::string get_output_precision_name(ngraph::Output<Node>& o) {
    auto elem_type = o.get_element_type();
    switch (elem_type) {
    case ::ngraph::element::Type_t::undefined:
        return "UNSPECIFIED";
    case ::ngraph::element::Type_t::f16:
        return "FP16";
    case ::ngraph::element::Type_t::f32:
        return "FP32";
    case ::ngraph::element::Type_t::bf16:
        return "BF16";
    case ::ngraph::element::Type_t::f64:
        return "FP64";
    case ::ngraph::element::Type_t::i8:
        return "I8";
    case ::ngraph::element::Type_t::i16:
        return "I16";
    case ::ngraph::element::Type_t::i32:
        return "I32";
    case ::ngraph::element::Type_t::i64:
        return "I64";
    case ::ngraph::element::Type_t::u8:
        return "U8";
    case ::ngraph::element::Type_t::u16:
        return "U16";
    case ::ngraph::element::Type_t::u32:
        return "U32";
    case ::ngraph::element::Type_t::u64:
        return "U64";
    case ::ngraph::element::Type_t::u1:
        return "BIN";
    case ::ngraph::element::Type_t::boolean:
        return "BOOL";
    default:
        NGRAPH_CHECK(false, "Unsupported precision in ", o);
        return "";
    }
}

std::string generate_unique_name(
    const std::unordered_set<std::string>& unique_names, std::string base_name,
    int suffix) {
    std::string new_name = base_name + std::to_string(suffix);
    if (unique_names.find(new_name) == unique_names.end()) {
        return new_name;
    } else {
        suffix++;
        return generate_unique_name(unique_names, base_name, suffix);
    }
}

// TODO: remove when CNNNetwork will be supporting not-unique names
std::string get_node_unique_name(std::unordered_set<std::string>& unique_names,
                                 const ngraph::Node* n) {
    std::string name = n->get_friendly_name();
    if (unique_names.find(name) != unique_names.end()) {
        name = generate_unique_name(unique_names, name, 0);
    }
    unique_names.insert(name);
    return name;
}

bool is_exec_graph(const ngraph::Function& f) {
    // go over all operations and check whether performance stat is set
    for (const auto& op : f.get_ops()) {
        const auto& rtInfo = op->get_rt_info();
        if (rtInfo.find("execTimeMcs") != rtInfo.end()) {
            return true;
        }
    }
    return false;
}

bool resolve_dynamic_shapes(const ngraph::Function& f) {
    const auto & f_ops = f.get_ordered_ops();
    if (std::all_of(f_ops.begin(), f_ops.end(),
            [](std::shared_ptr<Node> results) { return !results->is_dynamic(); })) {
        return false;
    }

    auto f_clone = ngraph::clone_function(f);
    const auto & f_clone_ops = f_clone->get_ordered_ops();
    NGRAPH_CHECK(f_ops.size() == f_clone_ops.size(), "Unexpected get_ordered_ops method behaviour");

    for (size_t id = 0; id < f_ops.size(); ++id) {
        auto & op = f_ops[id];
        auto & clone_op = f_clone_ops[id];

        if (auto op_subgraph = std::dynamic_pointer_cast<op::util::SubGraphOp>(op)) {
            resolve_dynamic_shapes(*op_subgraph->get_function());
        }

        op->validate_and_infer_types();
        clone_op->validate_and_infer_types();

        // dynamic_to_static function converts dynamic dimensions to static using
        // upperbound (get_max_length) dimension value.
        auto dynamic_to_static = [](const PartialShape & shape) -> PartialShape {
            if (shape.is_static() || shape.rank().is_dynamic()) {
                return shape;
            }
            auto out_shape = PartialShape::dynamic(shape.rank());
            for (size_t i = 0; i < shape.rank().get_length(); ++i) {
                const auto & in_dim = shape[i];
                out_shape[i] = (in_dim.is_dynamic() ? Dimension(in_dim.get_max_length()) : in_dim);
            }
            return out_shape;
        };

        OutputVector replacements(clone_op->get_output_size());
        if (!clone_op->constant_fold(replacements, clone_op->input_values())) {
            for (size_t output_id = 0; output_id < clone_op->get_output_size(); ++output_id) {
                clone_op->set_output_type(output_id, clone_op->output(output_id).get_element_type(),
                        dynamic_to_static(clone_op->output(output_id).get_partial_shape()));
                op->set_output_type(output_id, clone_op->output(output_id).get_element_type(),
                        clone_op->output(output_id).get_partial_shape());
            }
        } else {
            for (size_t output_id = 0; output_id < clone_op->get_output_size(); ++output_id) {
                op->set_output_type(output_id, replacements[output_id].get_element_type(),
                        replacements[output_id].get_partial_shape());
            }

            for (size_t i = 0; i < replacements.size(); ++i) {
                auto node_output = clone_op->output(i);
                auto replacement = replacements.at(i);
                if (replacement.get_node_shared_ptr() && (node_output != replacement)) {
                    node_output.replace(replacement);
                }
            }
        }
    }
    return true;
}

void ngfunction_2_irv10(pugi::xml_document& doc,
                        std::ostream& bin_file,
                        const ngraph::Function& f,
                        const std::map<std::string, ngraph::OpSet>& custom_opsets) {
    const bool exec_graph = is_exec_graph(f);

    pugi::xml_node netXml = doc.append_child("net");
    netXml.append_attribute("name").set_value(f.get_friendly_name().c_str());
    netXml.append_attribute("version").set_value("10");
    pugi::xml_node layers = netXml.append_child("layers");

    const std::unordered_map<ngraph::Node*, int> layer_ids =
        create_layer_ids(f);
    std::unordered_set<std::string> unique_names;

    bool has_dynamic_shapes = resolve_dynamic_shapes(f);

    for (const auto& n : f.get_ordered_ops()) {
        ngraph::Node* node = n.get();

        NGRAPH_CHECK(layer_ids.find(node) != layer_ids.end(), "Internal error");
        // <layers>
        pugi::xml_node layer = layers.append_child("layer");
        layer.append_attribute("id").set_value(layer_ids.find(node)->second);
        layer.append_attribute("name").set_value(
            get_node_unique_name(unique_names, node).c_str());
        auto layer_type_attribute = layer.append_attribute("type");
        if (!exec_graph) {
            layer.append_attribute("version").set_value(
                get_opset_name(node, custom_opsets).c_str());
        }
        // <layers/data>
        pugi::xml_node data = layer.append_child("data");

        // <layers/data> general attributes
        std::string node_type_name{node->get_type_name()};
        if (exec_graph) {
            visit_exec_graph_node(data, node_type_name, node);
        } else {
            XmlSerializer visitor(data, bin_file, node_type_name);
            NGRAPH_CHECK(node->visit_attributes(visitor),
                         "Visitor API is not supported in ", node);
        }
        layer_type_attribute.set_value(
            translate_type_name(node_type_name).c_str());

        const auto data_attr_size =
            std::distance(data.attributes().begin(), data.attributes().end());
        if (data_attr_size == 0) {
            layer.remove_child(data);
        }

        int port_id = 0;
        // <layers/input>
        if (node->get_input_size() > 0) {
            pugi::xml_node input = layer.append_child("input");
            for (auto i : node->inputs()) {
                NGRAPH_CHECK(i.get_partial_shape().is_static(),
                             "Unsupported dynamic input shape in ", node);

                pugi::xml_node port = input.append_child("port");
                port.append_attribute("id").set_value(port_id++);
                for (auto d : i.get_shape()) {
                    pugi::xml_node dim = port.append_child("dim");
                    dim.append_child(pugi::xml_node_type::node_pcdata)
                        .set_value(std::to_string(d).c_str());
                }
            }
        }
        // <layers/output>
        if ((node->get_output_size() > 0) && !ngraph::op::is_output(node)) {
            pugi::xml_node output = layer.append_child("output");
            for (auto o : node->outputs()) {
                NGRAPH_CHECK(o.get_partial_shape().is_static(),
                             "Unsupported dynamic output shape in ", node);

                pugi::xml_node port = output.append_child("port");
                port.append_attribute("id").set_value(port_id++);
                port.append_attribute("precision")
                    .set_value(get_output_precision_name(o).c_str());
                for (auto d : o.get_shape()) {
                    pugi::xml_node dim = port.append_child("dim");
                    dim.append_child(pugi::xml_node_type::node_pcdata)
                        .set_value(std::to_string(d).c_str());
                }
            }
        }
    }
    // <edges>
    const std::vector<Edge> edge_mapping = create_edge_mapping(layer_ids, f);
    pugi::xml_node edges = netXml.append_child("edges");
    for (auto e : edge_mapping) {
        pugi::xml_node edge = edges.append_child("edge");
        edge.append_attribute("from-layer").set_value(e.from_layer);
        edge.append_attribute("from-port").set_value(e.from_port);
        edge.append_attribute("to-layer").set_value(e.to_layer);
        edge.append_attribute("to-port").set_value(e.to_port);
    }
    // move back dynamic shapes
    if (has_dynamic_shapes) {
        f.validate_nodes_and_infer_types();
    }
}

}  // namespace

// ! [function_pass:serialize_cpp]
// serialize.cpp
bool pass::Serialize::run_on_function(std::shared_ptr<ngraph::Function> f) {
    // prepare data
    pugi::xml_document xml_doc;
    std::ofstream bin_file(m_binPath, std::ios::out | std::ios::binary);
    NGRAPH_CHECK(bin_file, "Can't open bin file: \"" + m_binPath + "\"");
    switch (m_version) {
    case Version::IR_V10:
        ngfunction_2_irv10(xml_doc, bin_file, *f, m_custom_opsets);
        break;
    default:
        NGRAPH_UNREACHABLE("Unsupported version");
        break;
    }

    // create xml file
    std::ofstream xml_file(m_xmlPath, std::ios::out);
    NGRAPH_CHECK(xml_file, "Can't open xml file: \"" + m_xmlPath + "\"");
    xml_doc.save(xml_file);
    xml_file.flush();
    bin_file.flush();

    // Return false because we didn't change nGraph Function
    return false;
}

namespace {

std::string valid_xml_path(const std::string &path) {
    NGRAPH_CHECK(path.length() > 4, "Path for xml file is to short: \"" + path + "\"");

    const char *const extension = ".xml";
    const bool has_xml_extension = path.rfind(extension) == path.size() - std::strlen(extension);
    NGRAPH_CHECK(has_xml_extension,
                 "Path for xml file doesn't contains file name with 'xml' extension: \"" +
                     path + "\"");
    return path;
}

std::string provide_bin_path(const std::string &xmlPath, const std::string &binPath) {
    if (!binPath.empty()) {
        return binPath;
    }
    assert(xmlPath.size() > 4); // should be check by valid_xml_path
    std::string bestPath = xmlPath;
    const char *const extension = "bin";
    const auto ext_size = std::strlen(extension);
    bestPath.replace(bestPath.size() - ext_size, ext_size, extension);
    return bestPath;
}

} // namespace

pass::Serialize::Serialize(const std::string& xmlPath,
                           const std::string& binPath,
                           pass::Serialize::Version version,
                           std::map<std::string, OpSet> custom_opsets)
    : m_xmlPath{valid_xml_path(xmlPath)}
    , m_binPath{provide_bin_path(xmlPath, binPath)}
    , m_version{version}
    , m_custom_opsets{custom_opsets}
{
}
// ! [function_pass:serialize_cpp]
