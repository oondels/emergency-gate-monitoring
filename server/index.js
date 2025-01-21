import express from "express";
import http from "http";
import { Server } from "socket.io";
import cors from "cors";
import { pool } from "./db.cjs";
import nodeMailer from "nodemailer";

const app = express();
const port = 3028;
const server = http.createServer(app);

app.use(cors({ origin: "*" }));
app.use(express.json());

const transporter = nodeMailer.createTransport({
  host: "smtp.gmail.com",
  port: 465,
  secure: true,
  auth: {
    user: process.env.EMAIL,
    pass: process.env.EMAIL_PASS,
  },
});

const io = new Server(server, {
  cors: {
    origin: "*",
  },
});

app.use((req, res, next) => {
  req.io = io;
  next();
});

let lastHeartbeatDoorOne = Date.now();
let lastHeartbeatDoorTwo = Date.now();

const heartbeatInterval = 150000;
io.on("connection", (socket) => {
  socket.on("heartbeat", (data) => {
    let doorNumber = Object.keys(data)[0];

    if (doorNumber === "2") {
      lastHeartbeatDoorTwo = Date.now();
    }
    if (doorNumber === "1") {
      lastHeartbeatDoorOne = Date.now();
    }
  });

  setInterval(() => {
    if (Date.now() - lastHeartbeatDoorTwo > heartbeatInterval) {
      console.log("Conexão com o portão 2 perdida");
      io.emit("connection_lost", { message: "door_two" });
    } else {
      socket.emit("door_connection", { door: "2" });
    }
    if (Date.now() - lastHeartbeatDoorOne > heartbeatInterval) {
      console.log("Conexão com o portão 1 perdida");
      io.emit("connection_lost", { message: "door_one" });
    } else {
      socket.emit("door_connection", { door: "1" });
    }
  }, heartbeatInterval);

  socket.on("door_status", async () => {
    const doorStatus1 = await pool.query(`
      SELECT status, date FROM portoes.portoes_emergencia
      WHERE portao = '1'
      ORDER BY date DESC
      LIMIT 1
    `);
    const doorStatus2 = await pool.query(`
      SELECT status, date FROM portoes.portoes_emergencia
      WHERE portao = '2'
      ORDER BY date DESC
      LIMIT 1
    `);

    socket.emit("door_status", {
      1: { status: doorStatus1.rows[0].status, date: doorStatus1.rows[0].date },
      2: { status: doorStatus2.rows[0].status, date: doorStatus2.rows[0].date },
    });
  });

  socket.on("last_openings", async () => {
    let lastOpenings = {
      1: [],
      2: [],
    };

    const queryOpenings = await pool.query(`
      WITH door_one AS (
        SELECT date, portao
        FROM portoes.portoes_emergencia
        WHERE status = true AND portao = '1'
        ORDER BY date DESC
        LIMIT 5
      ),
      door_two AS (
        SELECT date, portao
        FROM portoes.portoes_emergencia
        WHERE status = true AND portao = '2'
        ORDER BY date DESC
        LIMIT 5
      )
      SELECT * FROM door_one
      UNION ALL
      SELECT * FROM door_two;
    `);

    let openings = queryOpenings.rows.reduce(
      (acc, open) => {
        acc[open.portao].push(open.date);
        return acc;
      },
      { 1: [], 2: [] }
    );

    socket.emit("last_openings", openings);
  });
});

app.get("/", (req, res) => {
  res.status(200).send("Hello World, portas emergência.");
});

app.post("/portao_emerg", async (req, res) => {
  const { open, door, offline_mode, offline_openings } = req.body;
  const currentDate = new Date();

  const data = {
    status: open,
    date: currentDate.toLocaleString("pt-BR"),
    door: door,
  };
  let error = false;

  try {
    if (offline_mode) {
      if (offline_openings && offline_openings.length > 0) {
        let offlineError = false;
        for (let i = 0; i < offline_openings.length; i++) {
          const [datePart, timePart] = offline_openings[i].split(" ");
          const [day, month, year] = datePart.split("/");
          const [hours, minutes, seconds] = timePart.split(":");
          const date = new Date(year, month - 1, day, hours, minutes, seconds);
          const localDate = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
          const formattedDate = localDate.toISOString();

          const postOfflineOppenings = await pool.query(
            `
            INSERT INTO portoes.portoes_emergencia (portao, status, date)
            VALUES ($1, true, $2)
            RETURNING *
          `,
            [door, formattedDate]
          );

          if (postOfflineOppenings.rows.length === 0) {
            console.error("Erro ao postar abertura offline.");
            offlineError = true;
          }
        }
        if (offlineError) {
          return res.status(500).json({ message: "Erro ao salvar dados no banco de dados." });
        } else {
          return res.status(200).json({ message: "Dados enviados com sucesso." });
        }
      }
    } else {
      req.io.emit("portao_emerg", data);

      const doorStatus = await pool.query(
        `
        SELECT * FROM portoes.portoes_emergencia
        WHERE portao = $1
        ORDER BY date DESC
        LIMIT 1
      `,
        [door]
      );

      if (doorStatus.rows.length === 0) {
        console.error("Portão não encontrado");
        return res.status(404).json({ error: "Portão não encontrado." });
      }

      const updateDataBase = async (status) => {
        let query = `
          INSERT INTO portoes.portoes_emergencia (portao, status, date)
          VALUES ($1, ${status}, NOW() AT TIME ZONE 'America/Sao_Paulo')
          RETURNING *
        `;

        const postQuery = await pool.query(query, [door]);

        if (postQuery.rows.length === 0) {
          console.error("Erro ao salvar dados no banco de dados.");
          error = true;
        }
      };

      if (doorStatus.rows[0].status === false && open === true) {
        await updateDataBase(true);

        await transporter
          .sendMail({
            to: "your_email@email.com",
            subject: `⚠️ Portão de Emergência Aberto ⚠️`,
            html: `
            <div style="font-family: Arial, sans-serif; color: #FF6F61; line-height: 1.6; max-width: 600px; margin: 0 auto; background-color: #f9f9f9; padding: 20px; border-radius: 10px; box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);">
              <div style="text-align: center; margin-bottom: 20px;">
                <h1 style="color: #FF6F61; font-size: 24px; margin: 0;">Portão de Emergência Aberto</h1>
              </div>

              <div style="background-color: #ffffff; padding: 20px; border-radius: 8px; border: 1px solid #e0e0e0;">
                <h2 style="color: #FF6F61; font-size: 20px; margin: 0 0 10px; text-align: center;"><strong>Automação</strong></h2>

                <h1 style="color: #0d9757; font-size: 22px; margin-bottom: 10px;">Mensagem:</h1>
                <p style="font-size: 16px; color: #555; background-color: #f4f4f4; padding: 15px; border-radius: 5px; border: 1px solid #ddd;">
                  O portão de emergência da ${data.door === "1" ? "Expedição" : "Doca"} foi aberto em ${
              data.date
            }. Por favor, verifique a situação imediatamente!
                </p>
              </div>

              <div style="text-align: center; margin-top: 30px; color: #777; font-size: 14px;">
                <p>Este e-mail foi gerado automaticamente. Por favor, não responda.</p>
              </div>
          </div>
            `,
          })
          .then(() => {
            return;
          })
          .catch((error) => {
            console.error("Erro ao enviar email de porta de emergência: ", error);
          });
      }
      if (doorStatus.rows[0].status === true && open === false) {
        await updateDataBase(false);
      }

      if (error) {
        return res.status(500).json({ error: "Erro ao salvar dados no banco de dados." });
      } else {
        return res.status(200).json({ message: "Informação recebida." });
      }
    }
  } catch (error) {
    console.error("Erro ao receber dados dos portões", error);
    res.status(500).send("Erro interno no servidor:", error);
  }
});

server.listen(port, () => {
  console.log("Server running on port:", port);
});
