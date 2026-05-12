#include "../sop_graph.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Houdini /obj-style subnet: a transform-only container that nests
            // an inner SopGraph. SopGraph::cook() detects subnet nodes by
            // kind() and (a) emits a synthetic EmittedActor for the subnet
            // itself (transform-only parent — isSubnetMarker == true), then
            // (b) recursively cooks the inner graph and stamps each emitted
            // child with this subnet's uid as parentNodeUid so apply_emitted
            // can wire Actor::addChild edges in the live scene.
            //
            // 0 inputs / 0 outputs — subnets don't participate in the
            // geometry-flow DAG; they're scene-organisation containers.
            // cook() returns empty geometry; the action is in the graph cook
            // recursion.
            class SubnetSop : public SopNode
            {
            public:
                explicit SubnetSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("name", "subnet"));
                    declareParam(Parameter::makeVec3("translate", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("rotate_euler_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("scale", Vec3(1.0f)));
                }

                std::string kind() const override { return "subnet"; }

                InputsAndOutputs ports() const override { return {}; }

                Geometry cook(std::span<const Geometry *const> /*inputs*/) const override
                {
                    return Geometry{};
                }

                SopGraph *innerGraph() override { return m_inner.get(); }
                const SopGraph *innerGraph() const override { return m_inner.get(); }

                // Caller is responsible for routing the inner graph's
                // nextUid() allocator to the outer root via
                // SopGraph::setRoot before this graph is exposed to mutators
                // (deserialization does this in a post-pass).
                void setInnerGraph(std::unique_ptr<SopGraph> g) override { m_inner = std::move(g); }

            private:
                std::unique_ptr<SopGraph> m_inner;
            };
        }

        void registerSubnetSop()
        {
            SopRegistry::instance().registerType(
                {"subnet", "Subnet", "Subnet",
                 /*inputs*/ {}, /*outputs*/ {},
                 /*params*/ {
                     {"name",             ParamType::String, "\"subnet\""},
                     {"translate",        ParamType::Vec3,   "[0, 0, 0]"},
                     {"rotate_euler_deg", ParamType::Vec3,   "[0, 0, 0]"},
                     {"scale",            ParamType::Vec3,   "[1, 1, 1]"}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<SubnetSop>(uid);
                });
        }
    }
}
