// SPDX-License-Identifier: GPL-3.0+

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "MCPBridge.h"
#include "CDVD/CDVDcommon.h"
#include "Debugger/DebuggerStep.h"
#include "QtHost.h"

#include "Achievements.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/DebugInterface.h"
#include "Host.h"
#include "VMManager.h"
#include "common/Error.h"

#include <QtCore/QByteArray>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtEndian>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

namespace
{
#ifdef _WIN32
	using Socket = SOCKET;
	constexpr Socket INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
	using Socket = int;
	constexpr Socket INVALID_SOCKET_HANDLE = -1;
#endif
	constexpr quint32 MAX_FRAME_SIZE = 1024 * 1024;
	constexpr int PROTOCOL_VERSION = 1;

	Socket s_socket = INVALID_SOCKET_HANDLE;
	std::mutex s_socket_mutex;
	std::thread s_worker;
	std::atomic_bool s_stopping{false};
	std::atomic_bool s_ui_ready{false};
	quint64 s_vm_generation = 0;

	using Reply = std::shared_ptr<std::promise<QJsonObject>>;
	struct PendingStep
	{
		Reply reply;
		BreakPointCpu cpu;
		u32 target;
	};
	std::optional<PendingStep> s_pending_step;

	bool SendFrame(Socket socket, const QJsonObject& object)
	{
		const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
		if (payload.size() > MAX_FRAME_SIZE)
			return false;

		QByteArray frame(sizeof(quint32), Qt::Uninitialized);
		qToBigEndian(static_cast<quint32>(payload.size()), frame.data());
		frame.append(payload);
		qsizetype sent = 0;
		while (sent < frame.size())
		{
#ifdef MSG_NOSIGNAL
			constexpr int FLAGS = MSG_NOSIGNAL;
#else
			constexpr int FLAGS = 0;
#endif
			const int count = static_cast<int>(send(socket, frame.constData() + sent,
				static_cast<int>(frame.size() - sent), FLAGS));
			if (count <= 0)
				return false;
			sent += count;
		}
		return true;
	}

	bool ReadExact(Socket socket, char* dest, int size)
	{
		while (size > 0)
		{
			const int count = static_cast<int>(recv(socket, dest, size, 0));
			if (count <= 0)
				return false;
			dest += count;
			size -= count;
		}
		return true;
	}

	QJsonObject Failure(const QString& error)
	{
		return {{QStringLiteral("error"), error}};
	}

	QJsonObject Result(const QJsonValue& value)
	{
		return {{QStringLiteral("result"), value}};
	}

	QString Hex(quint64 value, int width)
	{
		return QStringLiteral("0x%1").arg(value, width, 16, QLatin1Char('0'));
	}

	bool ParseHex(const QString& value, quint64& result)
	{
		const QStringView text = value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ?
		                             QStringView(value).sliced(2) :
		                             QStringView(value);
		if (text.isEmpty())
			return false;
		for (const QChar ch : text)
		{
			if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
					(ch.toLower() >= QLatin1Char('a') && ch.toLower() <= QLatin1Char('f'))))
				return false;
		}
		bool valid;
		result = text.toULongLong(&valid, 16);
		return valid;
	}

	bool GetRange(const QJsonObject& args, int length, u32& address)
	{
		quint64 value;
		if (length <= 0 || !ParseHex(args.value(QStringLiteral("address")).toString(), value) ||
			value > UINT32_MAX || value + static_cast<unsigned int>(length) - 1 > UINT32_MAX)
			return false;
		address = static_cast<u32>(value);
		return true;
	}

	QJsonObject MemoryCommand(DebugInterface& cpu, const QString& name, const QJsonObject& args)
	{
		constexpr int MAX_BYTES = 65536;
		const bool write = name == QStringLiteral("write_memory");
		const bool string = name == QStringLiteral("read_string");
		QByteArray bytes;
		int length = args.value(QStringLiteral("length")).toInt();
		if (write)
		{
			if (!cpu.isCpuPaused())
				return Failure(QStringLiteral("Pause the VM before writing memory"));
			const QByteArray data = args.value(QStringLiteral("data")).toString().toLatin1();
			bytes = QByteArray::fromHex(data);
			if (bytes.toHex() != data.toLower())
				return Failure(QStringLiteral("Data must contain complete hexadecimal bytes"));
			length = static_cast<int>(bytes.size());
		}
		u32 address;
		if (length > MAX_BYTES || !GetRange(args, length, address))
			return Failure(QStringLiteral("Invalid memory range or length"));
		if (write)
		{
			// Validate the entire range before writing so an unmapped page cannot cause a partial write.
			QByteArray existing(length, Qt::Uninitialized);
			if (!cpu.ReadBytes(address, existing.data(), length) || !cpu.WriteBytes(address, bytes.constData(), length))
				return Failure(QStringLiteral("Memory range is not writable"));
			return Result(QJsonObject{{QStringLiteral("written"), length}});
		}
		if (string)
		{
			bytes.reserve(length);
			for (int offset = 0; offset < length; offset++)
			{
				bool valid;
				const u8 byte = cpu.Read8(address + offset, &valid);
				if (!valid)
					return Failure(QStringLiteral("Memory address is not readable"));
				if (byte == 0)
					break;
				bytes.append(static_cast<char>(byte));
			}
			return Result(QString::fromUtf8(bytes));
		}
		bytes.resize(length);
		if (!cpu.ReadBytes(address, bytes.data(), length))
			return Failure(QStringLiteral("Memory range is not readable"));
		return Result(QString::fromLatin1(bytes.toHex()));
	}

	QJsonObject RegisterCommand(DebugInterface& cpu, bool write, const QJsonObject& args)
	{
		const QJsonValue category_value = args.value(QStringLiteral("category"));
		const int category = category_value.toInt(-1);
		if ((!category_value.isUndefined() && category < 0) || category >= cpu.getRegisterCategoryCount())
			return Failure(QStringLiteral("Invalid register category"));
		if (write)
		{
			if (!cpu.isCpuPaused())
				return Failure(QStringLiteral("Pause the VM before writing a register"));
			const int index = args.value(QStringLiteral("index")).toInt(-1);
			if (category < 0 || index < 0 || index >= cpu.getRegisterCount(category))
				return Failure(QStringLiteral("Invalid register index"));
			QString value = args.value(QStringLiteral("value")).toString();
			if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
				value.remove(0, 2);
			if (value.isEmpty() || value.size() > 32)
				return Failure(QStringLiteral("Invalid register value"));
			u128 reg = {};
			const QString padded = value.rightJustified(32, QLatin1Char('0'));
			quint64 high;
			quint64 low;
			if (!ParseHex(padded.left(16), high) || !ParseHex(padded.right(16), low))
				return Failure(QStringLiteral("Invalid register value"));
			reg._u64[0] = low;
			reg._u64[1] = high;
			cpu.setRegister(category, index, reg);
			return Result(true);
		}
		QJsonArray categories;
		const int first = category < 0 ? 0 : category;
		const int end = category < 0 ? cpu.getRegisterCategoryCount() : category + 1;
		for (int cat = first; cat < end; cat++)
		{
			QJsonArray registers;
			for (int index = 0; index < cpu.getRegisterCount(cat); index++)
			{
				const u128 value = cpu.getRegister(cat, index);
				registers.append(QJsonObject{{QStringLiteral("name"), QString::fromUtf8(cpu.getRegisterName(cat, index))},
					{QStringLiteral("value"), Hex(value._u64[1], 16) + Hex(value._u64[0], 16).sliced(2)}});
			}
			categories.append(QJsonObject{{QStringLiteral("category"), cat},
				{QStringLiteral("name"), QString::fromUtf8(cpu.getRegisterCategoryName(cat))},
				{QStringLiteral("registers"), registers}});
		}
		return Result(categories);
	}

	QJsonObject BreakpointCommand(DebugInterface& cpu, const QString& name, const QJsonObject& args)
	{
		const BreakPointCpu type = cpu.getCpuType();
		if (name == QStringLiteral("clear_all_breakpoints"))
		{
			CBreakPoints::ClearAllBreakPoints();
			CBreakPoints::ClearAllMemChecks();
			return Result(true);
		}
		if (name == QStringLiteral("list_breakpoints"))
		{
			QJsonArray entries;
			for (const BreakPoint& bp : CBreakPoints::GetBreakpoints(type, false))
			{
				entries.append(QJsonObject{{"address", Hex(bp.addr, 8)}, {"enabled", bp.enabled},
					{"condition", QString::fromStdString(bp.cond.expressionString)},
					{"description", QString::fromStdString(bp.description)}, {"hits", static_cast<qint64>(bp.totalHits)}});
			}
			return Result(entries);
		}
		if (name == QStringLiteral("list_watchpoints"))
		{
			QJsonArray entries;
			for (const MemCheck& check : CBreakPoints::GetMemChecks(type))
			{
				entries.append(QJsonObject{{"start", Hex(check.start, 8)}, {"end", Hex(check.end, 8)},
					{"flags", static_cast<int>(check.memCond)}, {"enabled", check.result != MEMCHECK_IGNORE},
					{"condition", QString::fromStdString(check.cond.expressionString)},
					{"description", QString::fromStdString(check.description)}, {"hits", static_cast<qint64>(check.totalHits)}});
			}
			return Result(entries);
		}

		const bool watch = name.endsWith(QStringLiteral("watchpoint"));
		quint64 start;
		if (!ParseHex(args.value(watch ? QStringLiteral("start") : QStringLiteral("address")).toString(), start) ||
			start > UINT32_MAX || (!watch && start % 4 != 0))
			return Failure(QStringLiteral("Invalid breakpoint or watchpoint address"));
		quint64 end = 0;
		if (watch && (!ParseHex(args.value(QStringLiteral("end")).toString(), end) || end <= start || end > UINT32_MAX))
			return Failure(QStringLiteral("Invalid exclusive watchpoint end address"));

		if (name == QStringLiteral("remove_breakpoint"))
		{
			CBreakPoints::RemoveBreakPoint(type, start);
			return Result(true);
		}
		if (name == QStringLiteral("remove_watchpoint"))
		{
			CBreakPoints::RemoveMemCheck(type, start, end);
			return Result(true);
		}

		BreakPointCond condition;
		condition.debug = &cpu;
		const QByteArray text = args.value(QStringLiteral("condition")).toString().toUtf8();
		if (text.size() > 4096 || text.contains('\0'))
			return Failure(QStringLiteral("Invalid breakpoint condition"));
		condition.expressionString = text.toStdString();
		std::string error;
		if (!text.isEmpty() && !cpu.initExpression(text.constData(), condition.expression, error))
			return Failure(QString::fromStdString(error));

		const std::string description = args.value(QStringLiteral("description")).toString().toStdString();
		if (watch)
		{
			const QString mode = args.value(QStringLiteral("mode")).toString(QStringLiteral("access"));
			MemCheckCondition flags;
			if (mode == QStringLiteral("read"))
				flags = MEMCHECK_READ;
			else if (mode == QStringLiteral("write"))
				flags = MEMCHECK_WRITE;
			else if (mode == QStringLiteral("access"))
				flags = MEMCHECK_READWRITE;
			else if (mode.compare(QStringLiteral("onchange"), Qt::CaseInsensitive) == 0)
				flags = static_cast<MemCheckCondition>(MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE);
			else
				return Failure(QStringLiteral("Invalid watchpoint mode"));

			CBreakPoints::AddMemCheck(type, start, end, flags, MEMCHECK_BREAK);
			CBreakPoints::ChangeMemCheck(type, start, end, flags, MEMCHECK_BREAK);
			CBreakPoints::ChangeMemCheckDescription(type, start, end, description);
			if (text.isEmpty())
				CBreakPoints::ChangeMemCheckRemoveCond(type, start, end);
			else
				CBreakPoints::ChangeMemCheckAddCond(type, start, end, condition);
		}
		else
		{
			CBreakPoints::AddBreakPoint(type, start);
			CBreakPoints::ChangeBreakPointDescription(type, start, description);
			if (text.isEmpty())
				CBreakPoints::ChangeBreakPointRemoveCond(type, start);
			else
				CBreakPoints::ChangeBreakPointAddCond(type, start, condition);
		}
		return Result(true);
	}

	QJsonObject SystemCommand(DebugInterface& cpu, const QString& name, const QJsonObject& args)
	{
		if (name == QStringLiteral("game_info"))
		{
			return Result(QJsonObject{{"title", QString::fromStdString(VMManager::GetTitle(false))},
				{"serial", QString::fromStdString(VMManager::GetDiscSerial())},
				{"version", QString::fromStdString(VMManager::GetDiscVersion())},
				{"crc", Hex(VMManager::GetCurrentCRC(), 8)}});
		}
		if (name == QStringLiteral("save_state") || name == QStringLiteral("load_state"))
		{
			const int slot = args.value(QStringLiteral("slot")).toInt(-1);
			if (slot < 0 || slot >= VMManager::NUM_SAVE_STATE_SLOTS)
				return Failure(QStringLiteral("Invalid save-state slot"));
			if (name == QStringLiteral("load_state"))
			{
				Error error;
				if (!VMManager::LoadStateFromSlot(slot, false, &error))
					return Failure(QString::fromStdString(error.GetDescription()));
			}
			else
			{
				std::string error;
				VMManager::SaveStateToSlot(slot, false, [&error](const std::string& value) { error = value; });
				if (!error.empty())
					return Failure(QString::fromStdString(error));
			}
			return Result(true);
		}

		QJsonArray entries;
		if (name == QStringLiteral("get_modules"))
		{
			for (const IopMod& module : DebugInterface::get(BREAKPOINT_IOP).GetModuleList())
			{
				entries.append(QJsonObject{{"name", QString::fromStdString(module.name)}, {"version", module.version},
					{"address", Hex(module.text_addr, 8)}, {"entry", Hex(module.entry, 8)},
					{"text_size", static_cast<qint64>(module.text_size)}, {"data_size", static_cast<qint64>(module.data_size)}});
			}
			return Result(entries);
		}
		if (name == QStringLiteral("get_threads"))
		{
			for (const auto& thread : cpu.GetThreadList())
			{
				entries.append(QJsonObject{{"id", static_cast<qint64>(thread->TID())}, {"pc", Hex(thread->PC(), 8)},
					{"entry", Hex(thread->EntryPoint(), 8)}, {"status", static_cast<int>(thread->Status())},
					{"wait", static_cast<int>(thread->Wait())}, {"priority", static_cast<qint64>(thread->Priority())}});
			}
			return Result(entries);
		}
		if (name == QStringLiteral("get_backtrace"))
		{
			if (!cpu.isCpuPaused())
				return Failure(QStringLiteral("Pause the VM before reading a backtrace"));
			const qint64 thread_id = args.value(QStringLiteral("thread_id")).toInteger(-1);
			for (const auto& thread : cpu.GetThreadList())
			{
				if ((thread_id >= 0 && thread_id != thread->TID()) ||
					(thread_id < 0 && thread->Status() != ThreadStatus::THS_RUN))
					continue;
				for (const auto& frame : cpu.StackTrace(thread.get()))
				{
					entries.append(QJsonObject{{"entry", Hex(frame.entry, 8)}, {"pc", Hex(frame.pc, 8)},
						{"sp", Hex(frame.sp, 8)}, {"size", frame.stackSize}});
				}
				return Result(entries);
			}
			return Failure(QStringLiteral("Guest thread not found"));
		}
		return Failure(QStringLiteral("Unknown emulator command"));
	}

	QJsonObject ProcessDebuggerCommand(const QString& name, const QJsonObject& args)
	{
		if (!VMManager::HasValidVM())
			return Failure(QStringLiteral("No active PS2 virtual machine"));
		if (Achievements::IsHardcoreModeActive())
			return Failure(QStringLiteral("Debugger access is unavailable in RetroAchievements Hardcore Mode"));
		const QString cpu_name = args.value(QStringLiteral("cpu")).toString(QStringLiteral("ee"));
		if (cpu_name != QStringLiteral("ee") && cpu_name != QStringLiteral("iop"))
			return Failure(QStringLiteral("CPU must be ee or iop"));
		DebugInterface& cpu = DebugInterface::get(cpu_name == QStringLiteral("ee") ? BREAKPOINT_EE : BREAKPOINT_IOP);
		if (name == QStringLiteral("read_memory") || name == QStringLiteral("write_memory") ||
			name == QStringLiteral("read_string"))
			return MemoryCommand(cpu, name, args);
		if (name == QStringLiteral("read_registers") || name == QStringLiteral("write_register"))
			return RegisterCommand(cpu, name == QStringLiteral("write_register"), args);
		if (name == QStringLiteral("disassemble"))
		{
			const int count = args.value(QStringLiteral("count")).toInt();
			u32 address;
			if (count <= 0 || count > 256 || !GetRange(args, count * 4, address) || address % 4 != 0)
				return Failure(QStringLiteral("Invalid disassembly address or instruction count"));
			QJsonArray instructions;
			for (int index = 0; index < count; index++)
			{
				const u32 pc = address + index * 4;
				bool valid;
				cpu.Read32(pc, &valid);
				if (!valid)
					return Failure(QStringLiteral("Instruction address is not readable"));
				instructions.append(QJsonObject{{QStringLiteral("address"), Hex(pc, 8)},
					{QStringLiteral("instruction"), QString::fromStdString(cpu.disasm(pc, true))}});
			}
			return Result(instructions);
		}
		if (name == QStringLiteral("evaluate"))
		{
			const QByteArray expression = args.value(QStringLiteral("expression")).toString().toUtf8();
			if (expression.isEmpty() || expression.size() > 4096 || expression.contains('\0'))
				return Failure(QStringLiteral("Invalid expression"));
			u64 value;
			std::string error;
			if (!cpu.evaluateExpression(expression.constData(), value, error))
				return Failure(QString::fromStdString(error));
			return Result(Hex(value, 16));
		}
		if (name == QStringLiteral("pause"))
		{
			cpu.pauseCpu();
			return Result(Hex(cpu.getPC(), 8));
		}
		if (name == QStringLiteral("continue"))
		{
			if (cpu.isCpuPaused())
			{
				for (const BreakPointCpu type : DEBUG_CPUS)
					CBreakPoints::SetSkipFirst(type, DebugInterface::get(type).getPC());
				cpu.resumeCpu();
			}
			return Result(true);
		}
		if (name.endsWith(QStringLiteral("breakpoint")) || name.endsWith(QStringLiteral("breakpoints")) ||
			name.endsWith(QStringLiteral("watchpoint")) || name.endsWith(QStringLiteral("watchpoints")))
			return BreakpointCommand(cpu, name, args);
		return SystemCommand(cpu, name, args);
	}

	QJsonObject MountDisc(const QJsonObject& args)
	{
		const QString filename = args.value(QStringLiteral("path")).toString();
		const QFileInfo image(filename);
		if (!image.isAbsolute() || !image.isFile() || !image.isReadable())
			return Failure(QStringLiteral("Provide an absolute path to a readable disc image"));

		const std::string path = image.absoluteFilePath().toStdString();
		if (!VMManager::IsDiscFileName(path))
			return Failure(QStringLiteral("Unsupported PCSX2 disc image format"));

		switch (VMManager::GetState())
		{
			case VMState::Running:
			case VMState::Paused:
				if (!VMManager::ChangeDisc(CDVD_SourceType::Iso, path))
					return Failure(QStringLiteral("PCSX2 could not open the disc image"));
				return Result(QJsonObject{{"path", image.absoluteFilePath()}, {"action", "swapped"}});
			case VMState::Shutdown:
			{
				if (!s_ui_ready.load())
					return Failure(QStringLiteral("Finish PCSX2 startup before mounting a disc"));
				auto boot = std::make_shared<VMBootParameters>();
				boot->filename = path;
				boot->source_type = CDVD_SourceType::Iso;
				g_emu_thread->startVM(std::move(boot));
				if (!VMManager::HasValidVM())
					return Failure(QStringLiteral("PCSX2 could not boot the disc image. Check its startup dialog for details."));
				return Result(QJsonObject{{"path", image.absoluteFilePath()}, {"action", "booted"}});
			}
			default:
				return Failure(QStringLiteral("Wait for the current VM transition before mounting a disc"));
		}
	}

	QJsonObject ProcessRequest(const QJsonObject& request)
	{
		const auto command = request.value(QStringLiteral("command")).toString();
		if (command == QStringLiteral("status"))
		{
			QJsonObject status;
			status.insert(QStringLiteral("active"), VMManager::HasValidVM());
			status.insert(QStringLiteral("paused"), VMManager::GetState() == VMState::Paused);
			status.insert(QStringLiteral("generation"), QString::number(s_vm_generation));
			return {{QStringLiteral("result"), status}};
		}
		const QJsonObject args = request.value(QStringLiteral("args")).toObject();
		if (command == QStringLiteral("mount_disc"))
			return MountDisc(args);
		return ProcessDebuggerCommand(command, args);
	}

	void StartStep(const QString& name, const QJsonObject& args, const Reply& reply)
	{
		if (!VMManager::HasValidVM() || Achievements::IsHardcoreModeActive() || VMManager::GetState() != VMState::Paused)
		{
			reply->set_value(Failure(QStringLiteral("Stepping requires a paused VM and available debugger")));
			return;
		}
		const QString cpu_name = args.value(QStringLiteral("cpu")).toString(QStringLiteral("ee"));
		if (cpu_name != QStringLiteral("ee") && cpu_name != QStringLiteral("iop"))
		{
			reply->set_value(Failure(QStringLiteral("CPU must be ee or iop")));
			return;
		}
		if (s_pending_step)
		{
			reply->set_value(Failure(QStringLiteral("A step is already pending")));
			return;
		}
		const BreakPointCpu type = cpu_name == QStringLiteral("ee") ? BREAKPOINT_EE : BREAKPOINT_IOP;
		DebugInterface& cpu = DebugInterface::get(type);
		const u32 target = DebuggerStep::Target(cpu, name == QStringLiteral("step_over"));
		for (const auto guest_cpu : DEBUG_CPUS)
			CBreakPoints::SetSkipFirst(guest_cpu, DebugInterface::get(guest_cpu).getPC());
		CBreakPoints::AddBreakPoint(type, target, true, true, true);
		s_pending_step = PendingStep{reply, type, target};
		cpu.resumeCpu();
	}

	void Run(Socket socket, unsigned short port, const QByteArray& token)
	{
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
			!SendFrame(socket, {{QStringLiteral("version"), PROTOCOL_VERSION},
								   {QStringLiteral("token"), QString::fromLatin1(token)}}))
		{
			return;
		}

		while (!s_stopping.load())
		{
			char header[sizeof(quint32)];
			if (!ReadExact(socket, header, sizeof(header)))
				break;
			const quint32 size = qFromBigEndian<quint32>(header);
			if (size == 0 || size > MAX_FRAME_SIZE)
				break;
			QByteArray payload(size, Qt::Uninitialized);
			if (!ReadExact(socket, payload.data(), static_cast<int>(size)))
				break;
			const QJsonDocument document = QJsonDocument::fromJson(payload);
			if (!document.isObject())
				break;

			auto reply = std::make_shared<std::promise<QJsonObject>>();
			auto result = reply->get_future();
			Host::RunOnCPUThread([request = document.object(), reply] {
				if (s_stopping.load())
					return;
				const QString command = request.value(QStringLiteral("command")).toString();
				if (command == QStringLiteral("step") || command == QStringLiteral("step_over"))
					StartStep(command, request.value(QStringLiteral("args")).toObject(), reply);
				else
					reply->set_value(ProcessRequest(request));
			});
			const QString command = document.object().value(QStringLiteral("command")).toString();
			const bool step = command == QStringLiteral("step") || command == QStringLiteral("step_over");
			const auto step_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
			bool step_timed_out = false;
			while (result.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
			{
				if (s_stopping.load())
					return;
				if (step && std::chrono::steady_clock::now() >= step_deadline)
				{
					Host::RunOnCPUThread([reply] {
						if (s_pending_step && s_pending_step->reply == reply)
						{
							CBreakPoints::ClearTemporaryBreakPoints();
							s_pending_step.reset();
						}
					});
					step_timed_out = true;
					break;
				}
			}
			if (s_stopping.load() || !SendFrame(socket, step_timed_out ?
															Failure(QStringLiteral("Step timed out before the VM paused")) :
															result.get()))
				break;
		}
	}
} // namespace

void MCPBridge::Start()
{
	const QByteArray token = qgetenv("PCSX2_MCP_TOKEN");
	bool valid = false;
	const ushort port = qgetenv("PCSX2_MCP_PORT").toUShort(&valid);
	qunsetenv("PCSX2_MCP_PORT");
	qunsetenv("PCSX2_MCP_TOKEN");
	if (!valid || !port || token.size() != 64)
		return;

#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return;
#endif
	s_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s_socket == INVALID_SOCKET_HANDLE)
	{
#ifdef _WIN32
		WSACleanup();
#endif
		return;
	}
#ifdef SO_NOSIGPIPE
	const int enabled = 1;
	setsockopt(s_socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
	s_stopping.store(false);
	s_worker = std::thread([socket = s_socket, port, token] {
		Run(socket, port, token);
		const std::lock_guard lock(s_socket_mutex);
#ifdef _WIN32
		closesocket(socket);
#else
		close(socket);
#endif
		s_socket = INVALID_SOCKET_HANDLE;
	});
}

void MCPBridge::OnUIReady()
{
	s_ui_ready.store(true);
}

void MCPBridge::Stop()
{
	if (!s_worker.joinable())
		return;

	s_stopping.store(true);
	{
		const std::lock_guard lock(s_socket_mutex);
		if (s_socket != INVALID_SOCKET_HANDLE)
		{
#ifdef _WIN32
			shutdown(s_socket, SD_BOTH);
#else
			shutdown(s_socket, SHUT_RDWR);
#endif
		}
	}
	s_worker.join();
#ifdef _WIN32
	WSACleanup();
#endif
}

void MCPBridge::OnVMStarting()
{
	++s_vm_generation;
}


void MCPBridge::OnGameChanged()
{
	++s_vm_generation;
}

void MCPBridge::OnVMPaused(int breakpoint_cpu, bool stepping)
{
	if (!s_pending_step)
		return;
	const auto step = std::move(*s_pending_step);
	s_pending_step.reset();
	if (!breakpoint_cpu)
		CBreakPoints::ClearTemporaryBreakPoints();
	DebugInterface& cpu = DebugInterface::get(step.cpu);
	const u32 pc = cpu.getPC();
	step.reply->set_value(Result(QJsonObject{{"pc", Hex(pc, 8)},
		{"reached_target", stepping && breakpoint_cpu == step.cpu && pc == step.target},
		{"instruction", QString::fromStdString(cpu.disasm(pc, true))}}));
}

void MCPBridge::OnVMDestroyed()
{
	if (!s_pending_step)
		return;
	s_pending_step->reply->set_value(Failure(QStringLiteral("VM stopped during step")));
	s_pending_step.reset();
}
